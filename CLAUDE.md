# KrautDrums — Claude Code context

## What this is
A 16-voice tonal drum machine for Ableton Move, modeled on the 1969 Elka Drummer One.
9 base voices + 7 variations, with 16 selectable rhythm patterns from the original,
master-bus modeled tape delay (Echoplex EP-3 / Binson Echorec / Roland RE-201)
+ multi-mode reverb (EMT 140 Dattorro plate / AKG BX20 dispersive spring / live echo chamber)
+ Attitude pre-FX studio chain (V72/V76 preamp+transformer + Studer A80 tape+head-bump
+ EQ + Synthi-style diode-ladder LPF + Phase 90 phaser)
+ EMT 156 / Neumann broadcast-limiter style bus compressor (feedback detection,
sub-ms attack), and 3-layer analog drift + diode-starve frequency drift simulation.

Schwung sound generator. API: `plugin_api_v2_t`. Language: C.
Voice architecture: 16 always-allocated drum voices, each = one or two high-Q biquad
bandpass resonators excited by a Plaits-style shaped trigger pulse (1 ms diode-clipped
hard pulse + 6 ms FM pulse for attack chirp), with per-block diode-starve drift on the
resonator centre frequency that follows the AD envelope.

## Repo structure
- `src/dsp/krautdrums.c` — all DSP logic (voices, sequencer, FX, MIDI, render_block)
- `src/module.json` — module metadata and version (must match git tag on release)
- `scripts/build.sh` — Docker ARM64 cross-compile (always use this)
- `scripts/install.sh` — deploys to Move via scp + fixes ownership
- `.github/workflows/release.yml` — CI: verifies version, builds, releases, updates release.json
- `design-spec.md` — full design with voice frequencies, pad layout, menu params
- `drummer_one_voices.h` — voice parameter table from schematic + SPICE analysis
- `voice_netlists/` — SPICE netlists per voice (for re-validation in LTSpice)

## Voice architecture

16 voices (9 base + 7 variations), all allocated up-front (no voice stealing — all sound
simultaneously). Each voice = 1 or 2 biquad bandpass resonators + AD envelope. Multi-resonator
voices are: Snare-drum, Short cymbal, Long cymbal, Cow bell (and their "2"/"Middle" variants).

Voice indices and frequencies are in `design-spec.md` and `drummer_one_voices.h`. The Bass-drum
(V0) is the only voice with f₀ confirmed from schematic + SPICE; others are formula-extrapolated
or canonical-drum-typical values that need ear-calibration against samples.

### Trigger excitation (Plaits-style shaped pulse)

A clean impulse delta sounds like a test tone; real bridged-T resonators are excited by a
shaped pulse with edge content. Reference: Émilie Gillet's `analog_bass_drum.h` in Plaits.

Each voice trigger sets up two state machines that run for the first ~6 ms after onset:

1. **Hard pulse** — `pulse_remaining = 44 samples` (1 ms). During pulse, value =
   `pulse_height` (3..10, scales with velocity). On the very last sample of the pulse,
   value drops by 1.0 (the falling-edge contribution). After the hard pulse, value
   exponentially decays (`*= 1 - 1/9` per sample, ~0.2 ms RC tail).
2. **FM pulse** — `fm_pulse_remaining = 264 samples` (6 ms) of `fm = 1.0`. After the FM
   pulse ends, `retrig_pulse = -0.8` (a dip), then slowly decays back to 0 over ~50 ms.

Per sample, the exciter is built as:
```
v->pulse_lp += (1/5) * (pulse - v->pulse_lp)        # ~0.1 ms LPF
exciter = voice_diode((pulse - v->pulse_lp) + pulse * 0.044)
v->fm_pulse_lp += (1/5) * (fm_pulse_raw - v->fm_pulse_lp)
fm = v->fm_pulse_lp * 1.7 * VOICES[vi].fm_amount
in = exciter * (1 + fm) - v->retrig_pulse * 0.2
```

`voice_diode(x)` is asymmetric: positive values pass linearly, negative values pass
through `0.7·(2x)/(1+|2x|)` (soft-saturated). This adds the 2nd-harmonic content that a
real bridged-T circuit's trigger transistor produces. Per-voice `fm_amount` ranges
0.05 (cymbals/claves) to 0.45 (Bass 2) — kicks/toms get the biggest chirp.

Per-voice `drift_amt` is read in render_block's per-block update loop:
```
f_eff = f0_trig * (1 - drift_amt * (1 - env))
biquad_set_bpf(&v->res_a, f_eff, Q_a, SAMPLE_RATE)
```
This is the diode-starve frequency drift — pitch decays with the envelope. Range 0.005
(cymbals — barely perceptible) to 0.07 (Bass 2 — strong "thump" downward slide).
Both `f0_a_trig` and `f0_b_trig` are stored per-voice with their per-trigger Gaussian
jitter already applied, so multi-resonator voices stay independently inharmonic across
triggers.

## Pad layout
- LEFT 16 pads (notes 36-39, 44-47, 52-55, 60-63) = 16 latching rhythm patterns
- RIGHT 16 pads (notes 40-43, 48-51, 56-59, 64-67) = 16 momentary voice triggers

Multiple rhythm pads can be active simultaneously — patterns OR together. Density knob
in General menu compensates for trigger pile-up when many patterns are active.

## Menus (4 pages, jog-wheel navigation)

### Menu 1 — KrautDrums (voice levels, 8 knobs)
8 group levels covering 9 voice families. Cymbals (Short + Long + Middle) share K8.
See `design-spec.md` for exact mapping.

### Menu 2 — FX (insert effects)
Tape delay (K1-K3, mode in General K5), reverb (K4-K6, mode in General K6),
**Bus Comp** (K7 — EMT 156 / Neumann broadcast-limiter macro), all-decay multiplier (K8).

### Menu 3 — Attitude (pre-FX studio chain coloration)
Stereo signal flow:
`in → V72 transformer (HP-coupled tanh) → V72 tube saturation (asym tanh)
    → A80 tape saturation (cubic clip) → A80 head-bump (peaking @120Hz, gain ∝ Tape)
    → A80 HF rolloff one-pole (18→8kHz with Tape) → HPF → Body shelf → Air shelf
    → Synthi diode-ladder LPF → Phase 90 phaser → out`

Knobs: **Drive** (K1, V72 stage drive 1×–4×), **Tape** (K2, A80 drive + head-bump gain
0..+2dB + HF rolloff 18→8kHz), **HPF** (K3, 30-500Hz), **Body** (K4, ±6dB low shelf @ 80Hz),
**Air** (K5, ±6dB high shelf @ 6kHz), **Cutoff** (K6, ladder 100Hz-15kHz),
**Reso** (K7, ladder feedback k=0..3.95), **Phaser** (K8, MXR Phase 90 — single knob
maps to BOTH rate (log 0.05→0.6Hz) AND depth+wet, so it speeds up as it deepens).

### Menu 4 — General
Tempo, sync mode, analog drift, limiter, delay type, reverb mode, master volume, density.
(Master Pitch knob was removed in favor of Reverb Mode — drum machines historically
don't have global transpose; per-voice tuning works the same way.)

## Critical constraints (from /move-synth)

- NEVER write to `/tmp` — use `/data/UserData/` on device
- NEVER allocate memory in `render_block` — all state lives in the instance struct
- NEVER call printf/log/mutex in `render_block`
- Output path: `modules/sound_generators/krautdrums/` (not audio_fx!)
- Files on Move must be owned by `ableton:users` — `scripts/install.sh` handles this
- `release.json` is auto-updated by CI — never edit manually
- Git tag `vX.Y.Z` must match `version` in `src/module.json` exactly
- C declaration order: constants, static arrays, and lookup tables must appear BEFORE functions that use them
- State serialization: every param must appear in both `get_param("state")` and `set_param("state")` paths;
  use `%f` for float, `%d` for int — mismatches cause undefined behavior on ARM64
- `get_param` MUST return -1 for unknown keys (not 0) — returning 0 breaks Master FX menu editing
- 8-field `plugin_api_v2_t` struct (`get_error` MUST exist as NULL between `get_param` and `render_block`)
- `ui_hierarchy` MUST be returned from `get_param` (memcpy pattern) — module.json alone is NOT enough for synths
- `chain_params` returned via memcpy (snprintf truncates large JSON for 16 voices × N params)
- `dsp.so` filename (not krautdrums.so)
- Enum params: get_param returns string name; set_param accepts both string and int

## Build & deploy
```bash
./scripts/build.sh          # Docker ARM64 cross-compile
./scripts/install.sh        # Deploy to move.local
```

## Release
Use the `/move-schwung-release` skill.

## Source / license notes

Original Elka Drummer One schematic analysis: see prior conversation summary
(transcript references in design-spec.md). All DSP code is original — no GPL
contamination from analyzing the 1969 schematic. License: MIT.

## Implementation notes

- Voice trigger = Plaits-style shaped pulse (NOT a clean impulse) — see "Trigger
  excitation" section above. Per-voice `fm_amount` and `drift_amt` in `VOICES[]` table.
- Voice level via group: voices indexed 0-15, group mapping in `VOICE_GROUP[16]`
- Master signal chain: `voices_sum × 4 → attitude (V72 xfmr + V72 tube + A80 tape +
  A80 head-bump + tape LPF + HPF + Body + Air + diode-ladder LPF + Phase 90)
  → bus_comp (FB-detected EMT 156) → delay (insert: linear xfade) → reverb (insert:
  linear xfade, stereo from Dattorro plate / Spring / Chamber) → master_vol → limiter → out`
- Mix knobs are **true linear crossfades**: `delay_mix=0.5` → 50/50 dry/wet,
  `delay_mix=1.0` → full wet. Same for `reverb_mix`. NOT send-style.
- Sequencer runs at 24 PPQN; step events at 4 PPQN (16th notes)
- Pattern data is hardcoded in `RHYTHMS[16]` const array — Vincent will refine pattern velocities by ear

### Per-block coefficient updates in render_block

To keep `cosf`/`sinf`/`tanf` out of the per-sample loop, render_block recomputes:
- HPF, Body shelf, Air shelf biquads (Attitude EQ, scales with current params)
- A80 tape head-bump biquad (gain scales with `tape_amount`)
- Tape post-rolloff one-pole coef (cutoff scales 18kHz→8kHz with `tape_amount`)
- Diode-ladder `g` (one-pole coef = `1 - exp(-2π·fc/fs)`) and `k` (resonance feedback)
- Phaser allpass coefficients `phaser_coef_l/r` (rate driven by `phaser_amount`)
- Delay wet head-bump biquads (per voicing — EP-3 / Echorec / RE-201)
- **Diode-starve drift:** for each *active* voice, recompute `biquad_set_bpf` with
  `f0_trig × (1 - drift_amt × (1 - env))`. ~16 cosf/sinf pairs per block max.

These updates run ONCE before the per-sample loop; per-sample work uses precomputed
coefficients and avoids transcendentals entirely (except `tanf` inside the diode-ladder
input/feedback clip, which is the cheap rational `fast_tanh` approximation).

### Reverb topologies

- **Plate (Dattorro 1997 figure-8 tank)** — Pre-delay → 4 input diffuser allpasses →
  two cross-coupled tanks each containing `mod_ap → delay1 → damping LPF → static_ap →
  delay2`. Output is 7-tap weighted sum per channel from both tanks → genuine stereo
  image from a mono input. Lengths from Dattorro's published values, scaled 29.7→44.1kHz.
  Approx 180 KB of buffer state.
- **Spring (Parker 2011 / Bilbao 2009 BX20 model)** — 6 dispersion allpasses (lengths
  31, 47, 73, 109, 157, 199; coef g=0.75) feeding the comb bank. Frequency-dependent
  group delay produces the recognizable downward chirp on transient hits.
- **Chamber (Conny Plank Meistersaal-style)** — 6-tap early reflection delay (~7-51 ms
  with 1/r gains) feeding the comb bank for diffuse tail.

`reverb_process` returns stereo; spring and chamber clone mono to both channels, plate
returns true stereo from the figure-8 tap arrangement.

### Bus comp character (EMT 156 / Neumann)

- **Feedback detection** (sidechain reads gain-reduced output, not input). Program-
  dependent attack/release; smoother on transient-heavy material.
- **Sub-millisecond attack range**: `attack_ms = 1 - amount * 0.95` (1 ms at min knob,
  0.05 ms at max). Drum transients < 1 ms long, so 8 ms attack of older code let
  everything pass uncompressed.
- 4:1 ratio, 0 dB → -12 dB threshold sweep, 150 ms release, +0..+3 dB makeup applied
  only when GR is active.

### Delay voicings

- **EP-3 (Tape):** Single tap, 0.45 ms wow + 0.10 ms flutter (sine LFO). FET asymmetric
  saturation, fb_cap = 0.93. Wet head-bump biquad: 9.5 kHz, +4 dB, Q=1.2 (FET preamp shimmer).
- **Echorec (Magnetic):** 4 taps at 1:2:3:4. Tube saturation. **Dual-sine wow LFO**:
  `sin(φ) + 0.3·sin(2φ + 0.3)` mimics the rotating drum's eccentricity. Wet head-bump
  biquad: 4 kHz, +1.5 dB, Q=0.9 (tube-stage mid-bump).
- **RE-201 (Space):** 3 taps at 1/3 : 2/3 : 1.0. **`fb_cap = 1.05`** — self-oscillates
  above ~95% feedback (the *Phaedra* sound). The in-loop `delay_saturate` tanh bounds
  the runaway. Wet head-bump: 6 kHz, +0.5 dB (near-flat preamp).

## Known TODOs (post-scaffold)

1. ~~Refine the 16 rhythm pattern step data — current scaffold has placeholder grooves~~ ✓ All 16 patterns filled in (Stage 4 refinement, panel-order)
2. Ear-tune voice f₀ values against AnalogAudio1 / OSL004 sample packs
3. ~~Verify Tom 1 / Tom 2 / Tom 3 pitch ordering~~ ✓ Tom 1 = 228 Hz (low), Tom 2 = 350 Hz (mid), Tom 3 = 723 Hz (high)
4. Tune Q values per voice for the right decay character
5. ~~Ear-tune Attitude curves: V72 preamp asymmetry, Studer tape head-bump, SVF resonance character, Phase 90 sweep depth/rate~~ ✓ V72 transformer + A80 head-bump + Synthi ladder + Phase 90 rate-from-amount all implemented; will need ear-tuning at first build
6. Refine pattern velocities by ear against original Drummer One reference recordings (Cluster's *Zuckerzeit*, Kraftwerk early albums, Hainbach demos)
7. Calibrate `fm_amount` and `drift_amt` per voice at first build — initial values are educated guesses based on Plaits' 808 BD model
