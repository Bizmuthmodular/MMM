// Copyright 2021 Emilie Gillet.
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
// Changes: ChordBank (plaits/dsp/chords/chord_bank.h/.cc) forked into
// ChordVoicing: removed the built-in 11-chord table, HysteresisQuantizer,
// and BufferAllocator dependency; the current chord's notes now come from
// the host plugin via SetNotes() instead of set_chord(). ComputeChordInversion
// is kept verbatim except SemitonesToRatio(x) -> exp2f(x/12.0f).
//
// -----------------------------------------------------------------------------
//
// Chord voicing: converts a 4-note chord (given as semitone offsets from
// the root) plus a 0..1 "inversion"/timbre parameter into 5 voice
// (ratio, amplitude) pairs, smoothly walking through inversions and octave
// transpositions as the parameter sweeps.

#ifndef PLAITS_CHORD_VOICING_H_
#define PLAITS_CHORD_VOICING_H_

#include <math.h>

#include "plaits_chord_osc.h"  // MAKE_INTEGRAL_FRACTIONAL

static const int kChordNumNotes = 4;
static const int kChordNumVoices = 5;

class ChordVoicing {
 public:
  ChordVoicing() { }
  ~ChordVoicing() { }

  void Init() {
    for (int i = 0; i < kChordNumNotes; ++i) {
      ratios_[i] = 1.0f;
    }
  }

  // semitones[4]: the chord's 4 note offsets from the root, in semitones.
  // Must be in ascending order for ComputeChordInversion's octave-doubling
  // logic to behave as a genuine chord inversion (see the caller in
  // Chords.cpp, which sorts before calling this).
  void SetNotes(const float semitones[kChordNumNotes]) {
    for (int i = 0; i < kChordNumNotes; ++i) {
      ratios_[i] = exp2f(semitones[i] * (1.0f / 12.0f));
    }
  }

  // Verbatim port of ChordBank::ComputeChordInversion (chord_bank.cc),
  // operating on this chord's own ratios_ instead of a selected row from a
  // built-in chord bank.
  int ComputeChordInversion(
      float inversion,
      float* ratios,
      float* amplitudes) {
    const float* base_ratio = ratios_;
    inversion = inversion * float(kChordNumNotes * kChordNumVoices);

    MAKE_INTEGRAL_FRACTIONAL(inversion);

    int num_rotations = inversion_integral / kChordNumNotes;
    int rotated_note = inversion_integral % kChordNumNotes;

    const float kBaseGain = 0.25f;

    int mask = 0;

    for (int i = 0; i < kChordNumNotes; ++i) {
      float transposition = 0.25f * static_cast<float>(
          1 << ((kChordNumNotes - 1 + inversion_integral - i) / kChordNumNotes));
      int target_voice = (i - num_rotations + kChordNumVoices) % kChordNumVoices;
      int previous_voice = (target_voice - 1 + kChordNumVoices) % kChordNumVoices;

      if (i == rotated_note) {
        ratios[target_voice] = base_ratio[i] * transposition;
        ratios[previous_voice] = ratios[target_voice] * 2.0f;
        amplitudes[previous_voice] = kBaseGain * inversion_fractional;
        amplitudes[target_voice] = kBaseGain * (1.0f - inversion_fractional);
      } else if (i < rotated_note) {
        ratios[previous_voice] = base_ratio[i] * transposition;
        amplitudes[previous_voice] = kBaseGain;
      } else {
        ratios[target_voice] = base_ratio[i] * transposition;
        amplitudes[target_voice] = kBaseGain;
      }

      if (i == 0) {
        if (i >= rotated_note) {
          mask |= 1 << target_voice;
        }
        if (i <= rotated_note) {
          mask |= 1 << previous_voice;
        }
      }
    }
    return mask;
  }

 private:
  float ratios_[kChordNumNotes];
};

#endif  // PLAITS_CHORD_VOICING_H_
