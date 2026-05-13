# KrautDrums — Design Spec

A 16-voice tonal drum machine for Ableton Move, modeled on the **Elka Drummer One (1969)** —
the Italian preset-rhythm box that produced Cluster, Kraftwerk, Harmonia, Roedelius, Tangerine
Dream, and Popol Vuh. Adds 7 voice variations and modern performance features while preserving
the original's distinctive bridged-T resonator character.

## Identity

- **Module ID:** `krautdrums`
- **Display name:** KrautDrums
- **Abbreviation:** `KRAUT`
- **Author:** Vincent Filliforme
- **License:** MIT
- **Language:** C
- **Component type:** sound_generator
- **API:** plugin_api_v2_t

## Voice architecture

Each voice is a **bridged-T resonator** modeled in DSP as a self-oscillating high-Q biquad
bandpass + AD envelope + level. Multi-resonator voices (snare, cymbals, cowbell) have two
parallel biquads mixed together, matching the original's stacked-resonator topology.

Voice parameters were derived from schematic analysis of the 1969 Drummer One, validated
in SPICE to ~1% on the formula `f₀ = 1/(2π·√(Rb·Rt·C1·C2))`. See `drummer_one_voices.h`
for the source values.

### 16 voices (panel order)

| # | Voice | f₀ (Hz) | f₀ secondary | Q | Decay (ms) | Notes |
|---|---|---|---|---|---|---|
| 0 | Bass-drum | 71.6 | — | 8 | 250 | Schematic-confirmed anchor |
| 1 | Conga | 103 | — | 12 | 200 | |
| 2 | Tom 1 | 228 | — | 10 | 250 | Low tom |
| 3 | Tom 2 | 350 | — | 10 | 260 | Mid tom |
| 4 | Claves | 2,210 | — | 25 | 30 | |
| 5 | Snare-drum | 193 | 310 | 6, 8 | 220 | Stacked resonators for buzz |
| 6 | Short cymbal | 4,074 | 5,400 | 30, 25 | 90 | |
| 7 | Long cymbal | 3,461 | 4,900 | 35, 30 | 1,200 | |
| 8 | Cow bell | 536 | 800 | 18, 18 | 350 | TR-808-style 540+800 pair |
| 9 | Bass-drum 2 | 60 | — | 6 | 180 | Tighter / shorter kick |
| 10 | Tom 3 | 723 | — | 11 | 220 | High tom — schematic-extracted (10nF + 10nF cap pair) |
| 11 | Conga 2 | 150 | — | 14 | 180 | Higher-pitched conga |
| 12 | Claves 2 | 2,800 | — | 28 | 25 | Brighter clave |
| 13 | Snare-drum 2 | 220 | 360 | 7, 9 | 200 | Cracker variant |
| 14 | Middle cymbal | 3,700 | 5,100 | 32, 28 | 500 | Decay between Short and Long |
| 15 | Cow bell 2 | 640 | 880 | 18, 18 | 320 | Higher cowbell |

## Pad layout (Move 4×8 = 32 pads)

```
ROW 4 (top):    [60][61][62][63] [64][65][66][67]
ROW 3:          [52][53][54][55] [56][57][58][59]
ROW 2:          [44][45][46][47] [48][49][50][51]
ROW 1 (bottom): [36][37][38][39] [40][41][42][43]
                ←─── LEFT 16 ───→ ←──── RIGHT 16 ────→
                  16 RHYTHMS         16 VOICES
```

### Left 16 — rhythm patterns (latching toggle, multi-touch combines)

The 16 Drummer One rhythms in panel order, mappable to any combination
simultaneously (matching the schematic's diode-OR matrix on board DR1+DR2).
All patterns share a 16-step grid (12-step for Waltz and Slow Rock); multiple
patterns active = OR-merged step events with `density` knob compensation.

| Pad note | Index | Rhythm | Time sig | Length |
|---|---|---|---|---|
| 36 | 0 | Waltz | 3/4 | 12 steps |
| 37 | 1 | Tango | 4/4 (habanera) | 16 |
| 38 | 2 | Polka | 2/4 × 2 | 16 |
| 39 | 3 | Paso Doble | 2/4 march × 2 | 16 |
| 44 | 4 | Swing | 4/4 swung | 16 |
| 45 | 5 | Slow | 4/4 ballad | 16 |
| 46 | 6 | Beguine | 4/4 Caribbean | 16 |
| 47 | 7 | Cha-Cha | 4/4 | 16 |
| 52 | 8 | Rumba | 4/4 (3-2 son clave) | 16 |
| 53 | 9 | Samba | 2/4 fast × 2 | 16 |
| 54 | 10 | Mambo | 4/4 (2-3 son clave) | 16 |
| 55 | 11 | Bossa-Nova | 4/4 (3-2 bossa clave) | 16 |
| 60 | 12 | Rock | 4/4 | 16 |
| 61 | 13 | Slow Rock | 12/8 triplet feel | 12 steps |
| 62 | 14 | Shake | 4/4 fast | 16 |
| 63 | 15 | R & Blues | 4/4 shuffle | 16 |

### Right 16 — voice triggers (one-shot)

Trigger a single voice per press. Velocity-sensitive. Pressing any pad also sets
`current_voice` to that voice index for any future per-voice editing.

| Pad note | Voice |
|---|---|
| 40 | Bass-drum |
| 41 | Conga |
| 42 | Tom 1 |
| 43 | Tom 2 |
| 48 | Claves |
| 49 | Snare-drum |
| 50 | Short cymbal |
| 51 | Long cymbal |
| 56 | Cow bell |
| 57 | Bass-drum 2 |
| 58 | Tom 3 |
| 59 | Conga 2 |
| 64 | Claves 2 |
| 65 | Snare-drum 2 |
| 66 | Middle cymbal |
| 67 | Cow bell 2 |

## Menus (Shadow UI navigation via jog wheel)

### Menu 1 — KrautDrums (voice levels)

8 knobs covering the 9 original-voice families, with cymbals sharing K8. Variations
inherit their family's level (e.g. Bass-drum 2 follows K1).

| Knob | Param key | Param name | Range | Default | Affects |
|---|---|---|---|---|---|
| 1 | `lvl_bass` | Bass | 0-1 | 0.85 | V0 (Bass), V9 (Bass2) |
| 2 | `lvl_conga` | Conga | 0-1 | 0.65 | V1 (Conga), V11 (Conga2) |
| 3 | `lvl_tom1` | Tom 1 | 0-1 | 0.70 | V2 (Tom1) |
| 4 | `lvl_tom2` | Tom 2 | 0-1 | 0.70 | V3 (Tom2), V10 (Tom3) |
| 5 | `lvl_claves` | Claves | 0-1 | 0.55 | V4 (Claves), V12 (Claves2) |
| 6 | `lvl_snare` | Snare | 0-1 | 0.80 | V5 (Snare), V13 (Snare2) |
| 7 | `lvl_cowbell` | Cow Bell | 0-1 | 0.60 | V8 (Cow Bell), V15 (Cow Bell2) |
| 8 | `lvl_cymbals` | Cymbals | 0-1 | 0.55 | V6 (Short), V7 (Long), V14 (Middle) |

### Menu 2 — FX (insert effects on master bus)

Signal flow: `voices → mix → attitude → bus_comp → delay → reverb → output`. The "All Decay" knob
multiplies every voice's envelope decay time, useful for building drone/wash textures.

| Knob | Param key | Param name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `delay_mix` | Delay Mix | 0-100% | 25% | dry/wet for delay |
| 2 | `delay_time` | Delay Time | 10-1000 ms (exp) | 280 ms | tape echo timing |
| 3 | `delay_fb` | Delay Fdbk | 0-95% | 35% | feedback amount |
| 4 | `reverb_mix` | Reverb Mix | 0-100% | 30% | dry/wet for reverb |
| 5 | `reverb_decay` | Reverb Dcy | 0-100% | 55% | reverb decay length |
| 6 | `reverb_tone` | Reverb Tone | 0-100% | 50% | dark→bright |
| 7 | `bus_comp` | Bus Comp | 0-100% | 0% | EMT 156 / Neumann broadcast limiter macro |
| 8 | `all_decay` | All Decay | 1.0×-4.0× | 1.0× | global envelope decay multiplier |

### Menu 3 — Attitude (pre-FX studio chain coloration)

Signal flow within the page: `in → preamp → tape → HPF → low-shelf (body) → high-shelf (air) → resonant LPF → 4-stage phaser → out`.
Inserts between the bus mix and bus compressor.

| Knob | Param key | Param name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `preamp_drive` | Drive | 0-100% | 30% | V72/V76 tube preamp asymmetric tanh |
| 2 | `tape_amount` | Tape | 0-100% | 25% | Studer A80 cubic-soft-clip + HF rolloff |
| 3 | `hpf_freq` | HPF | 30-500 Hz | 60 Hz | aggressive Butterworth high-pass |
| 4 | `eq_body` | Body | -6 to +6 dB | 0 dB | low shelf at 80 Hz |
| 5 | `eq_air` | Air | -6 to +6 dB | 0 dB | high shelf at 6 kHz (presence vs Cluster haze) |
| 6 | `filter_cutoff` | Cutoff | 100-15000 Hz | 15 kHz | resonant LPF (SVF, Mu-Tron / EMS Synthi character) |
| 7 | `filter_reso` | Reso | 0-100% | 20% | LPF Q (0.5-10, self-osc at max) |
| 8 | `phaser_amount` | Phaser | 0-100% | 0% | MXR Phase 90 / Maestro PS-1A 4-stage allpass macro |

### Menu 4 — General

| Knob | Param key | Param name | Range | Default | Notes |
|---|---|---|---|---|---|
| 1 | `tempo` | Tempo | 60-200 BPM | 120 | only when Tempo Mode = Free |
| 2 | `tempo_mode` | Tempo Mode | enum {Free, Sync} | Sync | host clock vs internal |
| 3 | `drift` | Analog Drift | 0-100% | 15% | per-trigger ±cents on each voice |
| 4 | `limiter` | Limiter | enum {Off, On} | On | output safety limiter |
| 5 | `delay_type` | Delay Type | enum {Tape, Magnetic, Space} | Tape | EP-3 / Echorec / RE-201 |
| 6 | `reverb_type` | Reverb Mode | enum {Plate, Spring, Chamber} | Plate | EMT 140 / AKG BX20 / Echo Chamber |
| 7 | `master_vol` | Master Vol | 0-1 | 0.85 | overall output level |
| 8 | `density` | Density | 0-100% | 100% | trigger probability when N rhythms active |

## Performance features

### Multi-pattern combination ("Tutto" mode)

Pressing multiple rhythm pads ORs their patterns in real time. Density (K8 General)
**probabilistically gates** trigger events as more patterns activate — events that
fire keep their full velocity, but fewer events fire overall. This is the krautrock-
appropriate behavior: thinning rather than quietening, like the original Drummer One's
diode-OR matrix produced full-amplitude triggers for overlapping patterns.

```
P_keep_trigger = density × (1 / sqrt(N_active_patterns))   if N > 1
P_keep_trigger = density                                    if N = 1
```

At density = 100% with one pattern, every step that the pattern requests fires.
At density = 80% with 8 patterns combined, roughly 28% of step events fire — but
each one at full punch, not at 28% velocity.

### Latching pad behavior

Rhythm pads (left 16) are LATCHING — first press activates, second press deactivates.
Voice pads (right 16) are MOMENTARY — fire on every press.

### Analog drift (3-layer model)

When > 0%, drift applies a frequency multiplier to each voice's resonator(s) using
three composed layers, modeling 1969 Italian ceramic capacitors and their slow
thermal/aging behavior:

1. **Slow continuous baseline** (±3 cents @ ~0.1 Hz random walk, always active when
   drift > 0%) — the whole machine slowly wanders during a take, like a tape
   transport's pitch wandering. Per-block update with slewed-target random walk.
2. **Per-trigger Gaussian jitter** (±25 cents at 100%, scaled by drift amount) —
   sum-of-3-uniforms approximates a Gaussian distribution: most hits cluster near
   nominal (~26% within ±5¢), occasional outliers at the tails (~8.5% beyond ±25¢)
   feel like real component instability. More musical than uniform random.
3. **Independent jitter per resonator** — multi-resonator voices (snare, cymbal,
   cowbell, claves) have res_a and res_b drift on **independent random draws**,
   modeling caps that aged separately. The resulting subtle inharmonicity adds life
   to otherwise statically-tuned multi-mode voices.

### Master pitch (removed)

Originally specced as a global ±1 octave transpose on the General page. Removed
in v0.1 to make room for the Reverb Mode enum — drum machines historically don't
have global transpose (TR-808/909, LinnDrum, etc.), and per-voice tuning works
the same way for any musical reframing. If desired later, a 5th menu page is the
natural home for it.

## Signal chain (block diagram)

```
[16 voices, parallel]
  │ each: trigger → biquad bandpass(es) → AD envelope → level → group sum
  ↓
mix bus (mono sum, all voice families summed by their group level)
  ↓
ATTITUDE (stereo split here)
  ├─ V72/V76 preamp (asymmetric tanh, even-harmonic emphasis)
  ├─ Studer A80 tape (cubic clip + HF rolloff scaling with drive)
  ├─ HPF (Butterworth, 30-500 Hz)
  ├─ Low-shelf @ 80 Hz (body, ±6 dB)
  ├─ High-shelf @ 6 kHz (air, ±6 dB)
  ├─ SVF resonant LPF (Mu-Tron / EMS Synthi character)
  └─ 4-stage Phase 90 phaser (90° L/R LFO offset, 0.4 Hz)
  ↓
BUS COMP (EMT 156 / Neumann broadcast limiter)
  └─ stereo-linked feedback compressor, 4:1 ratio, 8ms attack, 150ms release
  ↓
delay (multi-tap modeled: EP-3 / Echorec / RE-201)
  ↓
reverb (EMT 140 plate / AKG BX20 spring / Live chamber)
  ↓
master volume × master limiter (optional)
  ↓
[stereo int16 output]
```
```

## DSP technical notes

### Biquad bandpass coefficients

Standard cookbook BPF (constant 0dB peak), Direct Form II Transposed in code:
```c
float w0 = 2.0f * M_PI * f0 / Fs;
float alpha = sinf(w0) / (2.0f * Q);
float b0 = alpha;
float b1 = 0.0f;
float b2 = -alpha;
float a0 = 1.0f + alpha;
float a1 = -2.0f * cosf(w0);
float a2 = 1.0f - alpha;
// normalize: divide all b's and a1, a2 by a0
```

### Plaits-style shaped trigger excitation

A clean impulse delta sounds like a test tone. The original Drummer One's bridged-T
self-oscillation was excited by an RC-discharge spike with diode asymmetry, which is
what gave each voice its "instrument" feel rather than "sine ping" feel. We model this
the same way Émilie Gillet's Plaits 808 BD does
([analog_bass_drum.h](https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/drums/analog_bass_drum.h)):

1. **Hard pulse** (1 ms = 44 samples @ 44.1k): value = `pulse_height` ∈ [3, 10] scaled
   by velocity; on the very last sample value drops by 1.0 (the falling edge becomes part
   of the diode-shaped excitation). After the hard pulse, exponential RC tail
   (`*= 1 - 1/9` per sample, ~0.2 ms).
2. **Pulse LPF + asymmetric diode shaper:**
   ```c
   pulse_lp += (1/5) * (pulse - pulse_lp);             // 0.1 ms LPF
   exciter = voice_diode((pulse - pulse_lp) + pulse * 0.044);
   // voice_diode: x≥0 → x;  x<0 → 0.7·(2x)/(1+|2x|)
   ```
3. **FM pulse** (6 ms = 264 samples) lifts the resonator centre frequency at attack:
   ```c
   fm_pulse_lp += (1/5) * (raw_fm_pulse - fm_pulse_lp);   // smooth chirp edge
   fm = fm_pulse_lp * 1.7 * VOICES[v].fm_amount;
   in = exciter * (1 + fm) - retrig_pulse * 0.2;
   ```
   Per-voice `fm_amount` ranges 0.05 (cymbals/claves) to 0.45 (Bass 2). Kicks/toms
   get the biggest chirp.

### Diode-starve frequency drift

Real bridged-T circuits use diodes in the feedback path; their forward voltage drops
with current, so the resonator's effective center frequency drifts down as the AD
envelope decays. We model this with a per-block update of the biquad coefficients:
```c
for each active voice v:
    drift = 1 - VOICES[v].drift_amt * (1 - v->env);
    biquad_set_bpf(&v->res_a, v->f0_a_trig * drift, Q_a, Fs);
    if (v->f0_b_trig > 0)
        biquad_set_bpf(&v->res_b, v->f0_b_trig * drift, Q_b, Fs);
```
`f0_*_trig` are stored at trigger time with all per-trigger Gaussian jitter applied.
Per-voice `drift_amt` ranges 0.005 (cymbals) to 0.07 (Bass 2). The biquad's z1/z2
state is *not* reset between updates — only the coefficients change, so the resonator
slides smoothly in pitch as it rings.

The resonator natural decay τ ≈ Q / (π·f₀) is short for high-Q drum voices (~3 ms
for Long Cym Q=35, f=3.4 kHz), so the audible decay shape is dominated by the AD
envelope below.

### AD envelope

Simple exponential decay with very fast attack (< 1 ms — the shaped trigger pulse
provides the audible attack transient; the envelope just shapes amplitude tail):
```c
env *= decay_coef;  // computed from decay_time_seconds
```
where `decay_coef = expf(-1.0f / (decay_time * Fs))`.

### Sequencer

Master clock at 24 PPQN. Step rate = 4 PPQN (16th notes). On each step:
1. For each rhythm pattern that's active:
   - Look up step events for the active patterns
   - For each voice, compute combined velocity (max across patterns), then apply
     probabilistic gating with `density × 1/sqrt(N_active)` keep probability
   - If velocity > 0, trigger the voice

### Attitude — pre-FX studio chain

Inserted between the bus mix and the bus compressor. Stereo split happens here.

Signal flow:
```
in → V72 transformer → V72 tube saturation → A80 tape saturation
   → A80 head-bump biquad → tape HF rolloff → HPF → Body shelf → Air shelf
   → Synthi diode-ladder LPF → Phase 90 phaser → out
```

**V72 transformer stage** — HP-coupled fast tanh on the residue, blended with the
slow integrator. Adds the bottom-end "iron" feel that distinguishes a real V72 from
a plain asymmetric saturator:
```c
float hp = x - state;
state += 0.005 * hp;             // 35 Hz one-pole integrator
return fast_tanh(hp * 1.2) * 0.6 + state;
```

**V72 tube saturation** — DC-biased asymmetric tanh, 2nd-harmonic emphasis:
```c
float biased = x * preamp_pregain + 0.10;
return (fast_tanh(biased) - fast_tanh(0.10)) / preamp_pregain;
// preamp_pregain = 1 + drive*3 (1×..4×, 12 dB headroom)
```

**A80 tape saturation** — cubic soft-clip emphasising 3rd harmonic:
```c
float y = x * tape_pregain;
if      (y >  1.5) y =  1.0;
else if (y < -1.5) y = -1.0;
else               y = y - (y*y*y) / 6.75;     // cubic
return y / tape_pregain;
```

**A80 head-bump** — peaking biquad at 120 Hz, Q=1.5, gain `tape_amount × 2 dB`.
This is the missing low-mid thickness that makes audio actually feel "printed to
tape." Sourced from `jatinchowdhury18/AnalogTapeModel calcHeadBumpFilter()` shape,
A80-typical values.

**Tape HF rolloff** — one-pole LPF, cutoff sweeps 18 kHz → 8 kHz with `tape_amount`
(models head-bias-frequency loss and generation loss).

**EQ** — standard Audio EQ Cookbook biquads (HPF, low-shelf @ 80 Hz, high-shelf
@ 6 kHz). Per-block coefficient computation, no per-sample cosf/tanf.

**Synthi-style diode-ladder LPF** (replaces the previous ZDF SVF — period-correct
for Krautrock: VCS3, Faust's Wümme, Tangerine Dream):
```c
float fb  = fast_tanh(z[3] * 0.7) * 1.4;        // tanh-clipped feedback (Synthi diode)
float u   = in - k * fb;
u         = fast_tanh(u * 0.5) * 2.0;            // soft input clip (VCS3 grit)
z[0] += g * (u    - z[0]);
z[1] += g * (z[0] - z[1]);
z[2] += g * (z[1] - z[2]);
z[3] += g * (z[2] - z[3]);
return z[3];
```
where `g = 1 - exp(-2π·fc/Fs)` and `k = filter_reso × 3.95` (just below self-osc).

**Phase 90 phaser** — 4-stage 1-pole allpass cascade. Each allpass coefficient is
`a = (1-g)/(1+g)` where g = tan(π·f/Fs). The phaser amount knob controls BOTH:
- **rate** logarithmically: `phaser_rate_hz = 0.05 × 12^amount` → 0.05 Hz at amount=0,
  0.6 Hz at amount=1 (slow Krautrock zone biased — Cluster's *Sowiesoso* sits ~0.2 Hz)
- **depth + wet**: sweep_depth = `amount × 600 Hz`; `phaser_wet = amount`

L and R use 90° LFO phase offset for stereo width.

### Bus Compressor — EMT 156 / Neumann broadcast limiter

**Feedback detection** (sidechain reads the gain-reduced output, not input). This is
the EMT 156 / Neumann signature: program-dependent attack/release, smooth on
transient-heavy material.

**Sub-millisecond attack range** scaled with the knob: `attack_ms = 1 - amount × 0.95`
(1 ms at min knob, 0.05 ms at max). Drum transients are <1 ms, so the previous fixed
8 ms attack let everything pass uncompressed.

```c
float prev_l = (*l) * c->gain;                  // FB detection: read PREVIOUS output
float prev_r = (*r) * c->gain;
float det    = max(|prev_l|, |prev_r|);

float attack_ms   = 1.0f - amount * 0.95f;       // 1.0 .. 0.05 ms
float attack_coef = 1 - exp(-1 / (attack_ms * 0.001 * Fs));
float release_coef = 0.000151;                    // 150 ms

float coef = (det > env) ? attack_coef : release_coef;
env += coef * (det - env);

if (env > threshold) {                            // threshold = 1 - amount*0.75
    float over = env - threshold;
    target = (threshold + over * 0.25) / env;     // 4:1 ratio
    target *= 1 + amount * 0.4;                   // makeup only when GR active
}
gain += 0.04 * (target - gain);                   // 1-pole gain smoothing
out = in * gain;
```

### Multi-mode reverb

The reverb supports three authentic voicings selected by `reverb_type`. The dispatcher
returns stereo (L+R pointers); plate is natively stereo from cross-coupled tanks,
spring and chamber clone mono to both channels.

- **Plate — EMT 140** (Lahr, Germany, 1957). 4×8 ft suspended steel plate driven by
  a transducer, picked up by stereo contact pickups. Dense modal cluster, no early
  reflections, dispersive (HF travels faster). Heard on *Autobahn*, *Phaedra*,
  *Cluster*, *Neu! 75*.

  Modeled with the **Dattorro 1997 figure-8 tank** topology (JAES 45/9) — the same
  approach Mutable Instruments clouds and Faust dattorro_rev use:
  ```
  in → 5.4 ms pre-delay → 4 series input diffuser allpasses (g=0.75/0.75/0.625/0.625;
                                                              lengths 211/159/562/411)
     → split into TWO cross-coupled tanks. Each tank:
         [modulated_ap (g=-0.7) → delay1 → damping_lpf → static_ap (g=0.5) → delay2]
         tank A → delay2 output × decay → tank B input
         tank B → delay2 output × decay → tank A input
     Stereo output: 7-tap weighted sum from BOTH tanks per channel
       (Dattorro's published taps, scaled 29.7 → 44.1 kHz × 1.482)
  ```
  Tank lengths from Dattorro: A=996/6598/2667/5512, B=1322/6249/3936/4687.
  1 Hz LFO modulates the input allpasses by ±8 samples (Dattorro §3.3).

  This figure-8 topology gives true "plate density" from sample 0 — no comb-filter
  "chorus-y" signature like the Schroeder bank had.

- **Spring — AKG BX20** (Vienna, 1965). Twin **torsion-spring** tank with motional
  feedback damping. Distinct downward chirp from dispersive wave propagation in the
  spring waveguide.

  Modeled per **Parker 2011** (DAFx, *Spring Reverb Emulation Using Dispersive
  Allpass Filters*) and Bilbao 2009: 6 cascaded Schroeder allpass dispersion stages
  with delays spread across 30–200 samples (`{31, 47, 73, 109, 157, 199}`,
  mutually-prime), allpass coefficient g=0.75. Longer cascaded allpasses produce
  proper frequency-dependent group delay (HF arrives sooner than LF) — the
  recognizable BX20 chirp slope. Output feeds the Schroeder comb bank for diffuse
  tail. Brighter and rattlier than plate.

- **Chamber — Live Echo Chamber** (Conny Plank, Hansa Meistersaal style). Real-room
  feel: a **6-tap early reflection delay** at ~7, 13.5, 21, 28.5, 38, 51 ms with
  decreasing gains (1/r law), modeling discrete first-order bounces in a small
  ~4×8m basement/stairwell. The taps feed the comb bank + 2 series allpasses for
  the diffuse late field.

Sources: Dattorro 1997 *Effect Design Part 1: Reverberator and Other Filters* (JAES);
Parker 2011 *Spring Reverb Emulation* (DAFx); Bilbao 2009; Smith *Spring Reverb
Emulation Using Dispersive Allpass Filters* (AES 121); Schroeder 1962; Moorer 1979;
Eventide TVerb (Meistersaal modeling reference); BlakeC27 EMT 140 LTSpice analysis;
UA manuals.

Memory cost: ~180 KB extra for the Dattorro figure-8 buffers vs the original
Schroeder bank. Total instance memory ~734 KB, comfortably within Move's budget.

### Multi-mode tape delay

The delay supports three authentic voicings selected by `delay_type`. All three share:
cross-coupled L/R feedback (ping-pong), per-voicing tonal/feedback character, shared
base delay time (10–1115 ms exponential), shared feedback knob (0–95%), and a per-
voicing **wet head-bump peaking biquad** (replaces flat treble/bass scalars — the
preamp-coloration signature is a *peak*, not a shelf).

- **Tape — Maestro Echoplex EP-3** (1969 onward). Single tape head, FET preamp.
  Wow ~0.45 ms (sine LFO, ~0.7 Hz from worn pinch roller). Flutter ~0.10 ms.
  Asymmetric FET soft-clip in the feedback path. Wet head-bump: **9.5 kHz, +4 dB,
  Q=1.2** (the bright, percussive EP-3 signature). HF rolloff in feedback keeps
  repeats relatively bright (catalinbread: "bright, percussive repeats — NOT dark").
  fb_cap = 0.93. Source: BlakeC27 LTSpice analysis of EP-3 preamp +
  jatinchowdhury18/AnalogTapeModel head-bump model.

- **Magnetic — Binson Echorec** (1950s onward, Bonfiglio Bini). Multi-tap
  rotating-drum delay with **4 fixed playback heads** at 74, 148, 222, 296 ms
  (1:2:3:4 ratio of base time). Tube preamp (6× 12AX7) — warm, even-harmonic
  emphasis (sat_drive=1.7, sat_asymmetry=0.18). Wet head-bump: **4 kHz, +1.5 dB,
  Q=0.9** (tube-stage mid-bump). Drum is *far* more stable than tape, but has
  eccentricity wobble — modeled as a **dual-sine LFO**:
  ```c
  wow_mod = sin(φ) + 0.3 × sin(2φ + 0.3)
  ```
  This gives the recognizable Echorec "almost steady" character (the rotating drum's
  imperfect head spacing produces a fundamental + 2nd-harmonic offset, not a pure
  sinusoid). HF rolloff ~10-12 kHz from bias frequency. fb_cap = 0.91.
  Source: Effectrode service manual analysis; Grokipedia tap-time data.

- **Space — Roland RE-201 Space Echo** (1974). 3 fixed playback heads at
  1/3, 2/3, 1.0 of base time. Free-floating tape transport → most-modulated wow
  (~0.65 ms) and flutter (~0.18 ms). Solid-state preamp at 17 V — near-symmetric
  saturation. Wet head-bump: **6 kHz, +0.5 dB, Q=0.7** (near-flat preamp).

  **fb_cap = 1.05 (>1)** — the unit truly self-oscillates above ~95% feedback,
  which is the legendary RE-201 build-up sound (heard on *Phaedra*, *Stratosfear*).
  At low signal levels loop gain >1 so oscillation grows; at saturation the
  in-loop tanh `delay_saturate` limits to ~0.95 → controlled steady-state self-osc.

  Source: Roland service manual; FreeStompBoxes preamp circuit analysis.

The wet head-bump biquads are recomputed once per block when the voicing changes —
keeps `cosf`/`sinf` out of the per-sample loop.

## Critical constraints (from /move-synth)

- 8-field `plugin_api_v2_t` struct (get_error MUST exist as NULL)
- `get_param` returns `-1` for unknown keys
- `ui_hierarchy` returned from get_param via memcpy
- chain_params returned from get_param via memcpy
- `dsp.so` filename (not krautdrums.so)
- Install path: `modules/sound_generators/krautdrums/`
- `raw_midi: true` in module.json (need pad MIDI)
- Files owned by `ableton:users` after deploy
