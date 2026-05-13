# KrautDrums

> **A 16-voice tonal drum machine for [Ableton Move](https://www.ableton.com/move/), modeled on the 1969 Elka Drummer One.**

The Elka Drummer One is the Italian preset-rhythm box that haunted half of 1970s Krautrock — Cluster's *Zuckerzeit*, Harmonia, Roedelius solo records, early Kraftwerk, Tangerine Dream and Popol Vuh's quieter moments. Its sound is the bridged-T resonator: each "drum" is a passive notch network kicked into self-oscillation by a trigger transistor, then diode-clamped as it decays.

KrautDrums brings that sound to the Move, with the studio chain that those records were printed through: V72 preamps, Studer A80 tape, EMT 140 plate reverb, Maestro Echoplex echo, MXR Phase 90, EMS Synthi-style diode-ladder filter, EMT 156 broadcast limiter.

Built for the [Schwung](https://github.com/charlesvestal/schwung) framework (API v2).

---

## Features at a glance

- **16 always-allocated voices** — 9 original Drummer One voices + 7 hand-tuned variations
- **Bridged-T resonator DSP**, voice frequencies derived from schematic + SPICE analysis
- **Plaits-style shaped trigger excitation** — 1 ms diode-clipped pulse + 6 ms FM pulse, with diode-starve frequency drift coupled to the envelope (so kicks pitch-bend down as they decay, like the real circuit)
- **16 Drummer One rhythm patterns**, with **8 simultaneous rhythm slots** for OR-merged multi-pattern grooves
- **Attitude pre-FX studio chain**: V72 transformer + V72 tube + Studer A80 tape + head-bump + EQ + Synthi diode-ladder LPF + Phase 90 phaser
- **Multi-mode delay**: Maestro Echoplex EP-3, Binson Echorec, Roland RE-201 (with the famous RE-201 self-oscillation)
- **Multi-mode reverb**: EMT 140 (Dattorro figure-8 plate), AKG BX20 (Parker dispersive spring), Conny Plank–style live chamber
- **Tempo sync** for the delay — 12 tempo divisions, host-clock or free
- **EMT 156-style bus compressor** with hard-limit + makeup pump
- **3-layer analog drift** (slow walk + per-trigger Gaussian + per-resonator independent)
- **10 ms smoothing** on all 19 audio-rate knob parameters (no zipper noise on automation)

---

## Voices

| # | Voice | f₀ | Q | Decay | Notes |
|---|---|---|---|---|---|
| 0 | Bass-drum | 71.6 Hz | 8 | 250 ms | Schematic-confirmed anchor |
| 1 | Conga | 103 Hz | 12 | 200 ms | |
| 2 | Tom 1 | 228 Hz | 10 | 250 ms | Low tom |
| 3 | Tom 2 | 350 Hz | 10 | 260 ms | Mid tom |
| 4 | Claves | 2 210 Hz | 25 | 30 ms | |
| 5 | Snare | 193 + 310 Hz | 6, 8 | 220 ms | Stacked resonators |
| 6 | Short cymbal | 4 074 + 5 400 Hz | 30, 25 | 90 ms | |
| 7 | Long cymbal | 3 461 + 4 900 Hz | 35, 30 | 1 200 ms | |
| 8 | Cow bell | 536 + 800 Hz | 18, 18 | 350 ms | TR-808-style 540 + 800 pair |
| 9 | Bass-drum 2 | 60 Hz | 6 | 180 ms | Tighter / shorter kick |
| 10 | Tom 3 | 723 Hz | 11 | 220 ms | High tom |
| 11 | Conga 2 | 150 Hz | 14 | 180 ms | Higher conga |
| 12 | Claves 2 | 2 800 Hz | 28 | 25 ms | Brighter clave |
| 13 | Snare 2 | 220 + 360 Hz | 7, 9 | 200 ms | Cracker variant |
| 14 | Middle cymbal | 3 700 + 5 100 Hz | 32, 28 | 500 ms | Between Short and Long |
| 15 | Cow Bell 2 | 640 + 880 Hz | 18, 18 | 320 ms | Higher cowbell |

Voice frequencies were derived from analysis of the 1969 schematic. The Bass-drum is the only voice with f₀ confirmed against a SPICE simulation of the original circuit; the others are formula-extrapolated from the bridged-T notch frequency `f = 1 / (2π·√(R_b·R_t·C_1·C_2))` against schematic values, and were ear-tuned against AnalogAudio1 / OSL004 sample packs.

Per-voice `fm_amount` (attack chirp depth) is intentionally kept small (0.02–0.08), matching the actual Drummer One's behaviour — it has no separate pitch-envelope generator like the TR-808. The chirp comes entirely from diode coupling. Per-voice `drift_amt` (diode-starve drift coefficient) is 0.005 for cymbals/claves up to 0.07 for Bass-drum 2.

---

## Pad layout — Move Drum Kit template

KrautDrums is designed for the Move's Drum Kit template, which numbers pads as two 4×4 blocks:

```
ROW 4 (top):    [48][49][50][51] [64][65][66][67]    notes 48-51 + 64-67
ROW 3:          [44][45][46][47] [60][61][62][63]
ROW 2:          [40][41][42][43] [56][57][58][59]
ROW 1 (bot):    [36][37][38][39] [52][53][54][55]
                ←─── LEFT 16 ────→ ←─── RIGHT 16 ───→
                  VOICE TRIGGERS    RHYTHM TOGGLES
```

### Left 16 pads — voice triggers (momentary, velocity-sensitive)

Notes 36–51 trigger voices 0–15 (panel order: Bass-drum, Conga, Tom 1, Tom 2, Claves, Snare, Short cym, Long cym, Cow Bell, Bass 2, Tom 3, Conga 2, Claves 2, Snare 2, Mid cym, Cow Bell 2).

### Right 16 pads — rhythm pattern toggles (latching, slot-managed)

Notes 52–67 toggle the 16 Drummer One rhythms:

```
Top row:  64=Rock      65=Slow Rock  66=Shake     67=R&Blues
Row 3:    60=Rumba     61=Samba      62=Mambo     63=Bossa-Nova
Row 2:    56=Swing     57=Slow       58=Beguine   59=Cha-Cha
Bot row:  52=Waltz     53=Tango      54=Polka     55=Paso Doble
```

Pressing a right pad assigns its rhythm to the first free **rhythm slot** (up to 8 simultaneous). Pressing it again removes it. Slots are visible and editable on the Rhythms menu page.

---

## Menus

Navigate with the jog wheel. The plugin appears as **"KrautDrums"** in the picker; in-menu knob popups prefix with **"KD"**.

### Page 1 — Kraut (voice family levels)

8 knobs covering the 9 voice families. Cymbals (Short + Long + Middle) share knob 8.

### Page 2 — FX (insert effects, true linear crossfade)

| Knob | Param | Range / behaviour |
|---|---|---|
| 1 | Delay Mix | 0–100% (0% = bypass, 50% = 50/50 dry/wet, 100% = full wet) |
| 2 | Delay Time | 10–1000 ms exponential (free) or 1/64–1/1 tempo divisions (synced) |
| 3 | Delay Fdbk | 0–95% (capped by per-voicing fb_cap; RE-201 goes to 105% for self-oscillation) |
| 4 | Reverb Mix | 0–100% linear xfade |
| 5 | Reverb Dcy | 0–100% (decay range 0.10–0.95 cross-feedback gain — short room to long EMT plate) |
| 6 | Reverb Tone | 0–100% (dark → bright damping LPF in tail) |
| 7 | Bus Comp | 0–100% (hard limit + amount-scaled makeup, see below) |
| 8 | All Decay | 0–100% (scales BPF Q from 1× to 16× → drone/wash textures) |

**Menu-only on FX page:** Delay Sync, Delay Type, Reverb Mode.

### Page 3 — Attitude (pre-FX studio chain)

Stereo signal flow:
`in → V72 transformer (HP-coupled tanh) → V72 tube saturation (asym tanh) → A80 tape saturation (cubic clip) → A80 head-bump (peaking @120Hz, gain ∝ Tape) → A80 HF rolloff (18 → 5 kHz with Tape) → HPF → Body shelf → Air shelf → Synthi diode-ladder LPF → Phase 90 phaser → out`

| Knob | Param |
|---|---|
| 1 | Drive (V72 stage 1× – 6×) |
| 2 | Tape (A80 drive + head-bump + HF rolloff) |
| 3 | HPF (30 – 500 Hz Butterworth) |
| 4 | Body (±6 dB low shelf @ 80 Hz) |
| 5 | Air (±6 dB high shelf @ 6 kHz) |
| 6 | Cutoff (100 Hz – 15 kHz diode-ladder LPF) |
| 7 | Reso (ladder feedback k = 0…3.95) |
| 8 | Phaser (one knob: log rate 0.05 → 0.6 Hz **and** depth + wet, so it speeds up as it deepens) |

### Page 4 — General

Tempo, Tempo Mode, Drift, Limiter, M.Volume, Density. (Delay Type / Reverb Mode moved to the FX menu.)

### Page 5 — Rhythms (NEW)

8 enum knobs. Each picks a rhythm from the 17-option list (Off + 16 patterns). The sequencer ORs all non-Off slots — duplicates are merged into one trigger stream. Right pads stay in sync: pressing a pad places its rhythm in the first free slot, or clears it if already present.

---

## DSP highlights

### Voice excitation (Plaits-style shaped trigger pulse)

A clean impulse delta sounds like a test tone. Real bridged-T resonators are excited by a shaped pulse with edge content. Reference: Émilie Gillet's `analog_bass_drum.h` in [Plaits](https://github.com/pichenettes/eurorack).

Each trigger sets up two state machines:

1. **Hard pulse** — 44 samples (1 ms) at `pulse_height` (3..10, velocity-scaled), with a falling-edge contribution on the last sample, followed by a ~0.2 ms RC tail.
2. **FM pulse** — 264 samples (6 ms) modulates the resonator centre frequency by `fm_amount × 1.7` of its own value.

The per-sample exciter is built as:
```
pulse_lp += (1/5) · (pulse − pulse_lp)             // 0.1 ms LPF
exciter = voice_diode((pulse − pulse_lp) + pulse × 0.044)
fm = fm_pulse_lp × 1.7 × VOICES[v].fm_amount
in = exciter × (1 + fm) − retrig_pulse × 0.2
```

`voice_diode(x)` is asymmetric: positive values pass linearly, negative values pass through `0.7·(2x)/(1+|2x|)`. This adds the 2nd-harmonic content that a real bridged-T circuit's trigger transistor produces.

### Diode-starve frequency drift

Real bridged-T circuits use diodes in the feedback path; their forward voltage drops with current, so the resonator's centre frequency drifts down as the envelope decays. Modeled per-block:

```
f_eff = f0_trig × (1 − drift_amt × (1 − env))
biquad_set_bpf(&res, f_eff, Q, SAMPLE_RATE)
```

`f0_trig` carries the per-trigger Gaussian jitter, so multi-resonator voices stay independently inharmonic across triggers.

### All Decay → drone/wash

The audible decay of each voice is limited by the BPF ring time (τ = Q/(π·f₀)). To extend voices into pads, **the All Decay knob scales Q from 1× to 16×**. At max:

- Bass-drum: Q goes 8 → 128, τ from 35 ms → 567 ms (60 dB ≈ 3.9 s)
- Long Cym: Q goes 35 → 560, τ from 3.2 ms → 51 ms (60 dB ≈ 352 ms)

The intended side effect at high Q is that drum hits soften into pitched "pings" — exactly the drone/wash texture described in the original design.

### Delay voicings

| Voicing | Taps | Wow shape | Sat. | fb_cap | Wet head-bump |
|---|---|---|---|---|---|
| **Tape** (EP-3) | 1 | Sine ~0.7 Hz, 0.45 ms | FET asymmetric, drive 1.4 | 0.93 | 9.5 kHz, +4 dB, Q=1.2 |
| **Magnetic** (Echorec) | 4 (1:2:3:4) | **Dual-sine** `sin(φ)+0.3·sin(2φ+0.3)` — drum eccentricity | Tube even-harmonic, drive 1.7 | 0.91 | 4 kHz, +1.5 dB, Q=0.9 |
| **Space** (RE-201) | 3 (1/3:2/3:1.0) | Sine ~0.7 Hz, 0.65 ms (max) | Near-symmetric, drive 1.85 | **1.05** (self-oscillates) | 6 kHz, +0.5 dB, Q=0.7 |

The dual-sine wow LFO on Echorec mode mimics the rotating-drum geometry (dominant cycle + 2nd-harmonic offset from imperfect head spacing) — the difference between "tape wobble" and "drum eccentricity."

**RE-201 fb_cap = 1.05** means feedback above ~95% triggers controlled self-oscillation — the legendary *Phaedra* sound. The in-loop tanh `delay_saturate` bounds the runaway to a steady oscillating state.

### Reverb voicings

| Voicing | Topology | Output | Memory |
|---|---|---|---|
| **Plate** (EMT 140) | Dattorro 1997 figure-8 tank: pre-delay → 4 input diffusers → 2 cross-coupled tanks (mod_ap → delay1 → damping → static_ap → delay2) | True stereo from 7-tap weighted sum per channel | ~180 KB |
| **Spring** (BX20) | 6 cascaded Schroeder dispersion allpasses (lengths 31, 47, 73, 109, 157, 199; coef 0.75) → comb bank | Mono → cloned to L/R | ~5 KB |
| **Chamber** (Plank) | 6-tap early reflection delay (7–51 ms, 1/r gains) → comb bank | Mono → cloned to L/R | ~10 KB |

Decay range is 0.10–0.95 cross-feedback gain (Plate). At 0.10 the tail dies in ~250 ms; at 0.95 it rings ~10 s. The figure-8 cross-coupling gives true plate density from sample 0, distinct from the "chorus-y" character of a plain Schroeder bank.

### Bus compressor (EMT 156 / Neumann)

- **Feed-forward peak detection** (FF, not FB — for percussive material FF responds reliably)
- **Sub-millisecond attack** range: 1 ms → 0.05 ms with knob position
- **Hard peak limit (∞:1) above threshold** + **amount-scaled makeup (1× → 4×, +12 dB)**
- Threshold sweep: 0.40 → 0.02

Above threshold the signal is clamped to threshold and then boosted by makeup; below threshold only makeup applies. Result: heavy pumping at high settings, audible glue + level lift at moderate, transparent at 0%.

### Attitude — V72/V76 + A80 + Synthi + Phase 90

- **V72 transformer**: HP-coupled fast tanh on the residue, blended with the slow integrator (the iron-feel low-end thickening that distinguishes the V72 from a generic asymmetric saturator).
- **V72 tube**: DC-bias asymmetric tanh, 2nd-harmonic emphasis. No gain-canceling divide — driving harder makes it louder and saturated, like the real stage.
- **A80 tape**: cubic soft-clip (3rd-harmonic) + peaking biquad head-bump at 120 Hz (0 → +4 dB scaled with Tape) + one-pole HF rolloff (18 → 5 kHz scaled with Tape). Head-bump model from [`jatinchowdhury18/AnalogTapeModel`](https://github.com/jatinchowdhury18/AnalogTapeModel)'s `calcHeadBumpFilter()` shape.
- **Synthi diode ladder**: 4-stage cascaded one-pole with tanh-clipped feedback (Synthi VCS3 diode character) and tanh-soft input clipping (VCS3 grit). Period-correct for Krautrock — Faust's Wümme studio, Tangerine Dream, Cluster all used VCS3-class ladders.
- **Phase 90**: 4-stage 1-pole allpass cascade, 90° L/R LFO offset. Single Amount knob controls both **rate** (logarithmic 0.05 → 0.6 Hz, slow Krautrock zone biased) and **depth + wet** — knob fully replaces both ARTIST controls.

### Parameter smoothing

All 19 float knob parameters (8 voice levels, delay/reverb/comp/all_decay, attitude stage, master vol, density) are smoothed per-block with a 10 ms time constant. The audio-rate compute paths read the smoothed companion values, so knob movements and automation never produce zipper noise.

---

## Build & install

```bash
./scripts/build.sh         # Docker ARM64 cross-compile (first run ~3 min, then cached)
./scripts/install.sh       # SCP to move.local + chown to ableton:users
```

Override the device hostname with `MOVE_HOST=10.0.0.5 ./scripts/install.sh`.

**After a `module.json` change**, power-cycle the Move (the chain_host caches metadata at startup). Re-add to a slot. After a `dsp.so`-only change, remove + re-add the slot is enough.

---

## Sources & credits

- **Original Elka Drummer One schematic analysis** — 1969 Italian preset rhythm box, public domain due to age. SPICE validation of bridged-T notch formula by Vincent Filliforme.
- **Plaits voice excitation model** — `analog_bass_drum.h` by Émilie Gillet, MIT, [`pichenettes/eurorack`](https://github.com/pichenettes/eurorack).
- **CHOWTapeModel head-bump** — Jatin Chowdhury, GPL3, [`jatinchowdhury18/AnalogTapeModel`](https://github.com/jatinchowdhury18/AnalogTapeModel) (only the head-bump biquad parameterisation pattern was studied; no code copied).
- **Dattorro plate reverb** — Jon Dattorro, *Effect Design Part 1: Reverberator and Other Filters* (JAES 45/9, 1997).
- **BX20 spring dispersion** — Julian Parker, *Spring Reverb Emulation Using Dispersive Allpass Filters* (DAFx 2011); Stefan Bilbao (2009).
- **EMT 140 LTSpice analysis** — BlakeC27.
- **EP-3 / Echorec / RE-201 voicings** — service manuals + Effectrode / Catalinbread / FreeStompBoxes analyses.
- **Cytomic linear-trapezoidal SVF** — Andy Simper (2017).
- **EMS Synthi III diode-ladder topology** — Stilson/Smith ZDF ladder reference.
- **Schwung framework** — [Charles Vestal](https://github.com/charlesvestal/schwung).

All DSP code is original C. No GPL contamination from analysing the 1969 schematic.

## License

MIT — see [LICENSE](LICENSE).
