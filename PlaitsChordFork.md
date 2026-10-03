# PlaitsChordFork — Specification

Replace the 4-voice chord synthesis section of the Disting NT plugin **Chords**
(`Chords.cpp`, GUID `CHRD`) with a forked version of the Mutable Instruments
Plaits chord engine. This document is the authoritative spec for the change.
Follow it closely; where it says VERIFY, read the referenced upstream source
file and confirm the detail against real code rather than trusting this prose.

---

## 1. Goal and scope

**Why:** the current chord section (4 naive phase-accumulator oscillators with
FM/PM/AM cross-modulation and a wavefolder) sounds weak. The Plaits chord
engine — band-limited divide-down string-machine oscillators crossfaded into
integrated wavetables, with a 5-voice octave-spread voicing — sounds full and
clean. We adopt its sound and its parameter philosophy wholesale.

**What stays untouched:**
- The progression sequencer: 8 steps of scale degrees, advance/reset triggers,
  clock division, the generator (`generateProgression`), scale/root selection
  (`kScaleChordTypes`, `kScaleIntervals`, `chordIntervals`).
- `updateFrequencies()`'s *pitch* logic: step → degree → chord type → root →
  four chord-tone frequencies. These four frequencies remain the source of
  truth for chord MIDI output and for the bass/melody note pools
  (`updateNotePool`, `updateMelNotePool`), which must keep working exactly as
  now.
- The entire bass section and the entire melody section (oscillators, glide,
  swarm, LFOs, their params).
- Chord MIDI output (Chord1CH…Chord4CH) and ChordDensity rhythm gating —
  behavior preserved, implementation adapted (see §6, §7).

**What is removed:** every chord *timbre* parameter and its DSP:
OSC1–4 Level, OSC1–4 Shape, ChordAlgo, ChordWave, Detune, FMFreq, FMDepth,
Phase, LevelFreq, LevelMod, ShapeFreq, ShapeMod, AmpLFOShape, ShpLFOShape,
Inversion, the per-step Inv1–Inv8 placeholders, and the v20 compatibility
placeholders (kParamOctave, kParamVoiceLeading). The per-sample chord
oscillator code in `step()` (the `case 0..3` chordAlgo switch, `wfold`, the
chord FM phase accumulators `fp0..fp3`, phase offsets, the level/shape LFO
application to chord voices) is deleted.

**What is added:** two timbre parameters in the Plaits idiom — **ChordMorph**
(waveform) and **ChordTimbre** (voicing/inversion) — plus **ChordLevel**.
Decision already made: **clean break** on parameter layout; old presets will
not load meaningfully. Do not add compatibility placeholders.

---

## 2. Upstream source material

Repository: `pichenettes/eurorack` on GitHub (clone locally; work from a local
checkout). License for Plaits (STM32 project): **MIT**. Rules:

- Every vendored/adapted file keeps Émilie Gillet's original MIT copyright
  header, with a line added: "Adapted for the Disting NT by <author>, <year>.
  Changes: <one line>."
- Do not use the names "Mutable Instruments" or "Plaits" in the plugin's
  user-facing name/strings. Attribution lives in source comments and docs.

Files to read and adapt (paths relative to repo root):

| Upstream file | Role | Fate in fork |
|---|---|---|
| `plaits/dsp/engine/chord_engine.cc/.h` | Top-level render flow, registration table, wavetable list, voice crossfade | Re-implemented as our `PlaitsChordUnit` (§4) |
| `plaits/dsp/chords/chord_bank.cc/.h` | Chord table + `ComputeChordInversion` voicing | Voicing math kept verbatim; chord table and `set_chord` deleted (§5) |
| `plaits/dsp/oscillator/string_synth_oscillator.h` | Divide-down organ/string voice | Vendored near-verbatim |
| `plaits/dsp/oscillator/wavetable_oscillator.h` | Integrated-wavetable voice (`WavetableOscillator<128, 15>`) | Vendored near-verbatim |
| `plaits/dsp/oscillator/oscillator.h` | polyBLEP helpers (`ThisBlepSample`, `NextBlepSample`) used by string synth | Copy only the helpers actually referenced |
| `stmlib/dsp/dsp.h` | `ONE_POLE`, `CONSTRAIN`, `MAKE_INTEGRAL_FRACTIONAL`, `Interpolate`, etc. | Copy only the macros/inlines actually referenced |
| `stmlib/dsp/units.h` + stmlib resources | `SemitonesToRatio` (LUT-based) | **Replace with `exp2f(semitones / 12.0f)`** |
| `plaits/resources.cc` | `wav_integrated_waves` data | Extract only the 15 tables the chord engine uses (§4.3) |

VERIFY as you vendor: the exact set of stmlib helpers each oscillator pulls
in. Chase includes; copy the minimal closure.

---

## 3. How the Plaits chord engine works (ground truth to preserve)

Understanding this section is required before writing code. Line references
are to `chord_engine.cc` at master.

### 3.1 Render flow

`ChordEngine::Render(parameters, out, aux, size, already_enveloped)`:

1. One-pole smooths `morph` and `timbre` once per render block:
   `ONE_POLE(morph_lp_, parameters.morph, 0.1f)` (same for timbre). Note:
   Plaits calls Render with block size 24 at 48 kHz; the 0.1 coefficient is
   per-block. See §6.4 for how we handle NT block sizes.
2. `chords_.set_chord(parameters.harmonics)` selects the chord from the bank
   — **this call is what our fork deletes**; chord identity comes from the
   host plugin instead.
3. `registration = max(1.0f - morph_lp_ * 2.15f, 0.0f)` — the *lower* half of
   MORPH scans the registration table; above ~0.465 registration pins to 0…
   wait, note the inversion: registration goes from 1 (MORPH=0) down to 0
   (MORPH≈0.465). `ComputeRegistration` linearly interpolates between the 8
   preset rows of `registrations[8][6]` (amplitudes for saw@1×, square@1×,
   saw@2×, square@2×, saw@4×, square@4× — VERIFY the harmonic ordering in
   `string_synth_oscillator.h`).
4. `ComputeChordInversion(timbre_lp_, ratios, amplitudes)` (a `ChordBank`
   method) converts TIMBRE into **5 voice ratios + 5 voice amplitudes** from
   the 4 chord notes, and returns `aux_note_mask` (bitmask of voices routed
   to aux — the chord root, so hardware AUX carries the root alone).
5. `f0 = NoteToFrequency(parameters.note) * 0.998f` — normalized frequency
   (cycles/sample). The 0.998 slightly flattens the divide-down stack while
   the wavetable voice renders at `note_f0 * 1.004f` — a deliberate ±0.1%
   detune between the two generator families. **Keep both constants.**
6. Per voice `note` in 0..4:
   - `wavetable_amount = 50.0f * (morph_lp_ - fade_point[note])`, clamped
     0..1; `fade_point = {0.55, 0.47, 0.49, 0.51, 0.53}`. Each voice
     crossfades from divide-down to wavetable at its own MORPH position, so
     the transition is staggered, not a hard switch.
   - `divide_down_gain = 4.0f - note_f0 * 32.0f`, clamped 0..1 — fades the
     string-machine component out at high pitch (where its brightness would
     alias/harshen).
   - Renders `StringSynthOscillator` and/or `WavetableOscillator` additively
     into `out` or `aux` per `aux_note_mask`, each scaled by
     `note_amplitudes[note]` and its crossfade weight.
7. Tail: `out[i] += aux[i]; aux[i] *= 3.0f;` — the main output already
   contains the full 5-voice chord. **Our fork uses `out` only** and keeps
   the `out += aux` mix; drop the `aux *= 3` line and the aux output.

### 3.2 StringSynthOscillator (divide-down voice)

Models a top-octave-divider string/organ machine: a single phase accumulator
per voice generating **phase-locked saw and square components at 1×, 2×, and
4× the fundamental** (6 components), mixed by the 6 registration amplitudes,
with polyBLEP band-limiting on the discontinuities. `Render` takes the
normalized f0, the amplitude array (the `harmonics[]` computed from
registration; note `chord_engine.cc` appends 2 extra zero entries — VERIFY
signature), a gain, and accumulates into the output buffer. Vendor this class
essentially verbatim.

### 3.3 WavetableOscillator<128, 15> (wavetable voice)

15 single-cycle waves, 128 samples each, stored with 4 guard samples (132) in
**integrated** form (`wav_integrated_waves`). At render time the oscillator
interpolates the integrated table and **differentiates** the result
(scaled by 1/f0), recovering the original wave with interpolation error
spectrally shaped downward — cheap anti-aliasing without mip-maps. MORPH's
upper half scans the 15 tables with interpolation between neighbors
(`waveform = max((morph_lp_ - 0.535f) * 2.15f, 0.0f)`). Vendor verbatim;
it needs the specific integrated data, not raw waves — do not substitute
your existing `g_wt` melody wavetable data.

### 3.4 ComputeChordInversion (the TIMBRE voicing)

Lives in `chord_bank.cc`. Input: `inversion` in 0..1, the current chord's 4
note ratios. Output: 5 `(ratio, amplitude)` pairs and the aux mask. Semantics:
as TIMBRE rises, the chord walks through inversions and octave
transpositions; one chord tone is doubled in another octave (which tone, and
in which octave, shifts as inversion progresses); amplitudes fade voices
in/out so a TIMBRE sweep morphs smoothly instead of stepping. VERIFY the
exact algorithm from source and **keep it verbatim** — it is the musical
heart of the engine. The only permitted change inside it: any
`SemitonesToRatio(x)` → `exp2f(x * (1.0f / 12.0f))`.

---

## 4. The fork: new self-contained headers

Create three headers next to `Chords.cpp`. They may include `<math.h>`,
`<stdint.h>`, `<algorithm>` (header-only `std::min/max/fill` are fine on the
NT toolchain), and each other — **nothing else**. No stmlib includes, no
plaits includes, no BufferAllocator, no heap, no static
constructors/objects with runtime initialization (plain
`static const` data tables are fine).

### 4.1 `plaits_chord_osc.h`

- The copied stmlib macros/inlines (`ONE_POLE`, `CONSTRAIN`,
  `MAKE_INTEGRAL_FRACTIONAL`, `Interpolate`, polyBLEP helpers).
- `StringSynthOscillator` — vendored.
- `WavetableOscillator<wavetable_size, num_waves>` — vendored.

### 4.2 `plaits_chord_voicing.h`

The forked `ChordBank`, renamed `ChordVoicing`:

```cpp
class ChordVoicing {
 public:
  void Init();                                // no allocator
  void SetNotes(const float semitones[4]);    // replaces set_chord()
  int  ComputeChordInversion(float inversion,
                             float* ratios,      // [5]
                             float* amplitudes); // [5]
 private:
  float ratios_[4];   // exp2f(semitones[i] / 12)
  // + whatever state the upstream voicing math needs — VERIFY
};
```

`SetNotes` is called from `updateFrequencies()` whenever the progression step
changes. It receives the chord's intervals **in semitones relative to the
chord root**, taken from the existing `chordIntervals[chordType][i]` lookup
(after the existing `if (semis < 0) semis += 12;` normalization — the notes
handed to the voicing must be ascending within one octave, matching what the
upstream bank stores; VERIFY upstream note ordering conventions and match
them). The `ComputeChordInversion` body is upstream code, unmodified except
`SemitonesToRatio` → `exp2f`.

### 4.3 `plaits_chord_wavetables.h`

`static const int16_t chord_wavetables[15][132] = { ... };` plus a
`static const int16_t* const chord_wavetable_ptrs[15]` array to pass to the
oscillator. Generate this file with a small Python script run against the
local `eurorack` checkout: parse `plaits/resources.cc`, locate
`wav_integrated_waves`, and emit the 15 entries selected in
`chord_engine.cc`'s `WAVE(bank, row, column)` list, i.e. indices
`(bank*64 + row*8 + column)*132 .. +132` for:
`(2,6,1) (2,6,6) (2,6,4) (0,6,0) (0,6,1) (0,6,2) (0,6,7) (2,4,7) (2,4,6)
(2,4,5) (2,4,4) (2,4,3) (2,4,2) (2,4,1) (2,4,0)` — **in exactly this order**
(it is the MORPH scan order). Commit the generator script alongside the
output. ~4 KB of const data; it compiles into the plugin binary like any
other table (the plugin already carries larger tables).

### 4.4 `PlaitsChordUnit` (in `Chords.cpp` or a fourth header)

Replaces `ChordEngine`, minus the bank and the aux output:

```cpp
struct PlaitsChordUnit {
  StringSynthOscillator divide_down_voice_[5];
  WavetableOscillator<128, 15> wavetable_voice_[5];
  ChordVoicing voicing_;
  float morph_lp_, timbre_lp_;

  void Init();
  void SetNotes(const float semitones[4]);   // forwards to voicing_
  void Render(float root_f0,                 // rootHz / sampleRate
              float morph, float timbre,     // 0..1
              float* out, float* aux_scratch, size_t size);
};
```

`Render` is a line-for-line adaptation of `ChordEngine::Render` with:
`set_chord` removed; `NoteToFrequency(parameters.note)` replaced by the
passed-in `root_f0` (still `* 0.998f`); aux handling reduced to
`out[i] += aux_scratch[i]` at the end. Everything else — registration math,
fade points, divide_down_gain, the 1.004 wavetable detune, ONE_POLE
smoothing — identical to upstream.

---

## 5. Pitch handoff (the seam between "their" and "our" code)

`updateFrequencies()` keeps computing, per progression step:

- `chordType`, `noteClass`, `rootHz` (root fixed at octave 3) — unchanged.
- The four plain chord-tone frequencies into `d->osc_freq[0..3]` — unchanged
  **except**: delete the inversion offset (`if (i < inv) semis += 12;`) and
  the detune multiplication. `osc_freq` now means "the four un-voiced chord
  tones" and exists solely for (a) chord MIDI output and (b) nothing else —
  the note pools are built from scale/chord data independently
  (`updateNotePool` — verify it does not read `osc_freq`; from current code
  it does not).
- NEW: fill `float semis[4]` with the normalized `chordIntervals` values and
  call `unit.SetNotes(semis)`; store `d->chordRootF0 =
  rootHz / NT_globals.sampleRate`.

Audible pitches are then `root_f0 * ratios[voice]` inside the engine — they
will differ from `osc_freq` by the voicing's octave placement. That is by
design. **MIDI sends the plain tones** (`hzToMidi(d->osc_freq[v])`), never
the voiced pitches, so turning ChordTimbre does not spray note-off/on churn.

`kParamInversion` and `kParamDetune` are deleted; ChordTimbre replaces both
roles.

---

## 6. step() restructure (chord section only)

### 6.1 Block rendering

Replace the per-sample chord voice code with, once per `step()` call:

```
numFrames = numFramesBy4 * 4
unit.Render(d->chordRootF0, morph01, timbre01, chordBuf, auxBuf, numFrames)
for each frame: bus[out] += chordBuf[i] * gateEnv * levelVolts
```

`morph01` / `timbre01` come from the new params (0–100 → ×0.01), read once
per block like other params. CV modulation of morph/timbre is out of scope
for v1 (note as future work).

### 6.2 Scratch buffers

Two `float` buffers of the maximum step block size are needed (`chordBuf`,
`auxBuf`). Determine the correct maximum from the NT API headers
(`NT_globals` / API docs — VERIFY what the guaranteed max frames per step is
for the target firmware) and allocate them in the DTC struct via
`calculateRequirements`, alongside `PlaitsChordUnit` itself. Nothing may be
allocated at render time. If DTC is tight, SRAM is acceptable for the
buffers; keep the oscillator state in DTC.

### 6.3 Density gating and level

ChordDensity behavior is preserved: the existing per-clock random loop
(`chordDensityLoop`, `chordLoopPos`, `chordMidiGated`) stays as-is for MIDI.
For audio, replace the old per-voice `eg0..eg3` envelopes with **one** gate
envelope applied to the summed chord render: a smoothed gain that slews
toward `chordMidiGated ? 1 : 0`. Match the attack/release feel of the current
implementation (read the current `eg` slew constants and reuse them). Chord
level: new ChordLevel param → volts scale with the same one-pole smoothing
pattern used by `lv0Smoothed` etc. today. Calibrate the output scale so that
ChordLevel=default is comparable in loudness to the old engine at its
defaults (the engine's raw output is roughly ±1 for a full chord;
multiply into the same volts range the plugin used before, e.g. ~5 V peak at
100%).

### 6.4 Smoothing coefficient

Upstream smooths morph/timbre with `ONE_POLE(x, target, 0.1f)` per 24-sample
block. NT step blocks may differ in size. Scale the coefficient:
`coef = 1.0f - powf(0.9f, numFrames / 24.0f)` computed per step (or
precompute for the common block size). Do not skip this — it is what makes
knob sweeps zipper-free.

### 6.5 Deletions checklist for step()

Delete: `fp0..fp3` FM phases and their write-back; `phOff*` phase offsets;
`wfold`; the `chordAlgo` switch; per-voice `lv*Smoothed` (replaced by one
chord level); the level/shape LFO application to chord voices (the LFOs
themselves may remain if the melody section uses them — VERIFY before
deleting shared state); `osc_phase[4]` accumulators (oscillator state now
lives inside `PlaitsChordUnit`). Keep: advance/reset trigger handling, the
progression clocking, chord MIDI gate/note logic (§5), bass and melody code
untouched.

---

## 7. Parameter layout (clean break)

New chord-section parameters (suggested ranges; keep the plugin's existing
`_NT_parameter` conventions and page structure style):

| Param | Range | Default | Meaning |
|---|---|---|---|
| ChordMorph | 0–100 | 25 | 0–46: registration scan (square → saw → octave stacks → full ensemble); 47–100: wavetable scan (15 waves) |
| ChordTimbre | 0–100 | 30 | Voicing / inversion / octave spread (5-voice) |
| ChordLevel | 0–100 | 75 | Chord output level |
| ChordOutput | bus | keep current default | unchanged |
| ChordDensity | 0–100 | 75 | unchanged |
| Chord1CH…Chord4CH | 0–16 | 0 | unchanged |
| MIDIOut | DIN/USB | unchanged | unchanged |

Rebuild the parameter enum without the removed entries and without any
placeholders; renumber freely; update every page list
(`progressionPageParams`, `oscillatorsPageParams` → replaced by a small
`chordPageParams`, etc.), `parameterChanged()` (remove the osc level/gain
handling; add Morph/Timbre/Level if they need cached conversions), and the
`draw()` function if it references removed params. Bump the plugin's
version/description string: "4-voice chord oscillator" → e.g. "chord machine
(string/wavetable) with progression sequencer".

---

## 8. Disting NT constraints checklist (must all hold)

- Only `distingnt/api` headers from the NT API are included as external
  headers; all Plaits-derived code is vendored source in the plugin folder.
  This mirrors the precedent set by the `airwindows` ports inside the
  official `distingNT_API` repository.
- No heap allocation anywhere; all state sized in `calculateRequirements`
  (SRAM: `ChordsAlg` incl. `PlaitsChordUnit`? — decide: unit state is
  per-instance audio-rate state → put it in the DTC struct with the rest of
  the oscillator state; scratch buffers likewise, DTC preferred).
- No static objects with constructors; tables are `static const` POD only.
- No exceptions, no RTTI, no STL containers; `<algorithm>` value helpers ok.
- Placement-new construction pattern unchanged (`construct()` already does
  this); add explicit `Init()` calls for the new unit in `construct()`.
- Wavetable data is `const` (flash/binary), not copied to `g_wt`-style
  static RAM — it does not need the melody wavetable's runtime build step.
- Keep the existing `calculateStaticRequirements`/`initialise` melody
  wavetable machinery untouched.

---

## 9. Acceptance criteria

1. Builds with the NT plugin toolchain with no warnings introduced.
2. Progression behavior identical: same degrees, same scales, same
   advance/reset/clocking, same generator output for a given seed.
3. Chord MIDI output identical to before for the same progression (modulo
   the removed Inversion parameter's effect).
4. Bass and melody sections bit-identical in behavior.
5. ChordMorph at 0 sounds like a band-limited square chord; sweeping to ~50%
   passes through saw/ensemble registrations; upper half scans wavetables
   with no zipper noise and no audible aliasing at high roots.
6. ChordTimbre sweeps voicings smoothly — no clicks, no note retriggers, and
   five audible pitches (one doubled tone) at most positions.
7. ChordDensity gates the chord audio and MIDI as before.
8. CPU load of the plugin at default settings ≤ the previous version
   (measure with the NT's CPU display; the old per-sample chord code was
   heavier than this block-based engine is expected to be).

## 10. Future work (explicitly out of scope for v1)

- CV inputs / modulation for ChordMorph and ChordTimbre.
- Optional subtle per-voice detune (2-line change in `ChordVoicing` ratios)
  if the perfectly-locked stack feels too clean.
- Optional separate bus output carrying the isolated chord root (the aux
  mask makes this nearly free).
