// Copyright 2016 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// Adapted for the Disting NT by Andras Eichstaedt, 2026.
// Changes: ChordEngine (plaits/dsp/engine/chord_engine.h/.cc) forked into
// PlaitsChordUnit: removed the chord bank/set_chord call and the aux bus
// output (folded into the main output instead); root frequency and chord
// notes are supplied by the host plugin instead of EngineParameters; the
// ONE_POLE morph/timbre smoothing coefficient is scaled from the host's
// actual (variable) block size instead of Plaits' fixed 24-sample block.
//
// -----------------------------------------------------------------------------
//
// Chords: wavetable and divide-down organ/string machine.

#ifndef PLAITS_CHORD_UNIT_H_
#define PLAITS_CHORD_UNIT_H_

#include <stdint.h>
#include <algorithm>
#include <math.h>

#include "plaits_chord_osc.h"
#include "plaits_chord_voicing.h"
#include "plaits_chord_wavetables.h"

static const int kChordNumHarmonics = 3;

static const float kChordFadePoint[kChordNumVoices] = {
  0.55f, 0.47f, 0.49f, 0.51f, 0.53f
};

static const int kChordRegistrationTableSize = 8;
static const float kChordRegistrations[kChordRegistrationTableSize][kChordNumHarmonics * 2] = {
  { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f },  // Square
  { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },  // Saw
  { 0.5f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f },  // Saw + saw
  { 0.33f, 0.0f, 0.33f, 0.0f, 0.33f, 0.0f },  // Full saw
  { 0.33f, 0.0f, 0.0f, 0.33f, 0.0f, 0.33f },  // Full saw + square hybrid
  { 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f },  // Saw + high square harmo
  { 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.5f },  // Square + high square harmo
  { 0.0f, 0.1f, 0.1f, 0.0f, 0.2f, 0.6f },  // Saw+square + high harmo
};

static inline void ComputeChordRegistration(
    float registration,
    float* amplitudes) {
  registration *= (kChordRegistrationTableSize - 1.001f);
  MAKE_INTEGRAL_FRACTIONAL(registration);

  for (int i = 0; i < kChordNumHarmonics * 2; ++i) {
    float a = kChordRegistrations[registration_integral][i];
    float b = kChordRegistrations[registration_integral + 1][i];
    amplitudes[i] = a + (b - a) * registration_fractional;
  }
}

struct PlaitsChordUnit {
  StringSynthOscillator       divide_down_voice_[kChordNumVoices];
  WavetableOscillator          wavetable_voice_[kChordNumVoices];
  ChordVoicing                 voicing_;
  float                        morph_lp_;
  float                        timbre_lp_;

  void Init() {
    for (int i = 0; i < kChordNumVoices; ++i) {
      divide_down_voice_[i].Init();
      wavetable_voice_[i].Init();
    }
    voicing_.Init();
    morph_lp_ = 0.0f;
    timbre_lp_ = 0.0f;
  }

  void SetNotes(const float semitones[kChordNumNotes]) {
    voicing_.SetNotes(semitones);
  }

  // Line-for-line adaptation of ChordEngine::Render (chord_engine.cc):
  // - set_chord() deleted (chord identity comes from SetNotes(), above)
  // - NoteToFrequency(parameters.note) replaced by the passed-in root_f0
  // - aux output folded into out (out[i] += aux_scratch[i]) instead of
  //   being exposed on a separate bus; the `aux[i] *= 3.0f` upstream tail
  //   is dropped since aux_scratch has no external consumer here
  void Render(
      float root_f0,
      float morph,
      float timbre,
      float* out,
      float* aux_scratch,
      uint32_t size) {
    // Upstream smooths morph/timbre with ONE_POLE(x, target, 0.1f) once per
    // fixed 24-sample Plaits block. Scale the coefficient for the host's
    // actual (variable) block size so knob sweeps stay zipper-free.
    const float coef = 1.0f - powf(0.9f, float(size) / 24.0f);
    ONE_POLE(morph_lp_, morph, coef);
    ONE_POLE(timbre_lp_, timbre, coef);

    float harmonics[kChordNumHarmonics * 2 + 1];  // 7 entries; only 0..6 read
    float note_amplitudes[kChordNumVoices];
    float registration = std::max(1.0f - morph_lp_ * 2.15f, 0.0f);

    ComputeChordRegistration(registration, harmonics);
    harmonics[kChordNumHarmonics * 2] = 0.0f;

    float ratios[kChordNumVoices];
    int aux_note_mask = voicing_.ComputeChordInversion(
        timbre_lp_,
        ratios,
        note_amplitudes);

    std::fill(&out[0], &out[size], 0.0f);
    std::fill(&aux_scratch[0], &aux_scratch[size], 0.0f);

    const float f0 = root_f0 * 0.998f;
    const float waveform = std::max((morph_lp_ - 0.535f) * 2.15f, 0.0f);

    for (int note = 0; note < kChordNumVoices; ++note) {
      float wavetable_amount = 50.0f * (morph_lp_ - kChordFadePoint[note]);
      CONSTRAIN(wavetable_amount, 0.0f, 1.0f);

      float divide_down_amount = 1.0f - wavetable_amount;
      float* destination = (1 << note) & aux_note_mask ? aux_scratch : out;

      const float note_f0 = f0 * ratios[note];
      float divide_down_gain = 4.0f - note_f0 * 32.0f;
      CONSTRAIN(divide_down_gain, 0.0f, 1.0f);
      divide_down_amount *= divide_down_gain;

      if (wavetable_amount) {
        wavetable_voice_[note].Render(
            note_f0 * 1.004f,
            note_amplitudes[note] * wavetable_amount,
            waveform,
            chord_wavetable_ptrs,
            destination,
            size);
      }

      if (divide_down_amount) {
        divide_down_voice_[note].Render(
            note_f0,
            harmonics,
            note_amplitudes[note] * divide_down_amount,
            destination,
            size);
      }
    }

    for (uint32_t i = 0; i < size; ++i) {
      out[i] += aux_scratch[i];
    }
  }
};

#endif  // PLAITS_CHORD_UNIT_H_
