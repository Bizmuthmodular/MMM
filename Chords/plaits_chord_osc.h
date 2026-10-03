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
// Changes: vendored StringSynthOscillator and WavetableOscillator
// near-verbatim from plaits/dsp/oscillator/{string_synth,wavetable}_oscillator.h
// with the minimal closure of stmlib helpers (ONE_POLE, CONSTRAIN,
// MAKE_INTEGRAL_FRACTIONAL, ThisBlepSample/NextBlepSample, ParameterInterpolator)
// inlined below; no functional changes.
//
// -----------------------------------------------------------------------------
//
// Divide-down "string synth" oscillator and integrated-wavetable oscillator,
// as used by Plaits' chord engine.

#ifndef PLAITS_CHORD_OSC_H_
#define PLAITS_CHORD_OSC_H_

#include <stdint.h>
#include <algorithm>

// ---------------------------------------------------------------------------
// Minimal closure of stmlib/dsp helpers actually used below.
// (stmlib/dsp/dsp.h, stmlib/stmlib.h, stmlib/dsp/polyblep.h,
//  stmlib/dsp/parameter_interpolator.h)
// ---------------------------------------------------------------------------

#define MAKE_INTEGRAL_FRACTIONAL(x) \
  int32_t x ## _integral = static_cast<int32_t>(x); \
  float x ## _fractional = x - static_cast<float>(x ## _integral);

#define ONE_POLE(out, in, coefficient) out += (coefficient) * ((in) - out);

#define CONSTRAIN(var, min, max) \
  if (var < (min)) { \
    var = (min); \
  } else if (var > (max)) { \
    var = (max); \
  }

inline float ThisBlepSample(float t) {
  return 0.5f * t * t;
}

inline float NextBlepSample(float t) {
  t = 1.0f - t;
  return -0.5f * t * t;
}

// stmlib::ParameterInterpolator — linear ramp of a parameter across a
// render block, writing the final value back to persistent state on
// destruction.
class ParameterInterpolator {
 public:
  ParameterInterpolator() { }
  ParameterInterpolator(float* state, float new_value, uint32_t size) {
    Init(state, new_value, size);
  }

  ~ParameterInterpolator() {
    *state_ = value_;
  }

  inline void Init(float* state, float new_value, uint32_t size) {
    state_ = state;
    value_ = *state;
    increment_ = (new_value - *state) / static_cast<float>(size);
  }

  inline float Next() {
    value_ += increment_;
    return value_;
  }

 private:
  float* state_;
  float value_;
  float increment_;
};

// ---------------------------------------------------------------------------
// StringSynthOscillator (plaits/dsp/oscillator/string_synth_oscillator.h)
//
// A mixture of 7 sawtooth and square waveforms in the style of divide-down
// organs:
//
// [0]     [1]        [2]     [3]        [4]     [5]        [6]
// Saw 8', Square 8', Saw 4', Square 4', Saw 2', Square 2', Saw 1'
//
// Internally, this renders 4 band-limited sawtooths, from 8' to 1' from a
// single phase counter. The square waveforms are obtained by algebraic
// manipulations on the sawtooths, using the identity:
// Square 16' = 2 Sawtooth 16' - Sawtooth 8'
// ---------------------------------------------------------------------------

class StringSynthOscillator {
 public:
  StringSynthOscillator() { }
  ~StringSynthOscillator() { }

  inline void Init() {
    phase_ = 0.0f;
    next_sample_ = 0.0f;
    segment_ = 0.0f;

    frequency_ = 0.001f;
    saw_8_gain_ = 0.0f;
    saw_4_gain_ = 0.0f;
    saw_2_gain_ = 0.0f;
    saw_1_gain_ = 0.0f;
  }

  // unshifted_registration must have (at least) 7 valid entries; only
  // indices 0..6 are ever read (verified against the real upstream source —
  // the 8th entry some upstream call sites pad with is never touched here).
  inline void Render(
      float frequency,
      const float* unshifted_registration,
      float gain,
      float* out,
      uint32_t size) {
    frequency *= 8.0f;

    // Deal with very high frequencies by shifting everything 1 or 2 octave
    // down: Instead of playing the 1st harmonic of a 8kHz wave, we play the
    // second harmonic of a 4kHz wave.
    uint32_t shift = 0;
    while (frequency > 0.5f) {
      shift += 2;
      frequency *= 0.5f;
    }
    // Frequency is just too high.
    if (shift >= 8) {
      return;
    }

    float registration[7];
    std::fill(&registration[0], &registration[shift], 0.0f);
    std::copy(
        &unshifted_registration[0],
        &unshifted_registration[7 - shift],
        &registration[shift]);

    ParameterInterpolator fm(&frequency_, frequency, size);
    ParameterInterpolator saw_8_gain_modulation(
        &saw_8_gain_,
        (registration[0] + 2.0f * registration[1]) * gain,
        size);
    ParameterInterpolator saw_4_gain_modulation(
        &saw_4_gain_,
        (registration[2] - registration[1] + 2.0f * registration[3]) * gain,
        size);
    ParameterInterpolator saw_2_gain_modulation(
        &saw_2_gain_,
        (registration[4] - registration[3] + 2.0f * registration[5]) * gain,
        size);
    ParameterInterpolator saw_1_gain_modulation(
        &saw_1_gain_,
        (registration[6] - registration[5]) * gain,
        size);

    float phase = phase_;
    float next_sample = next_sample_;
    int segment = segment_;
    while (size--) {
      float this_sample = next_sample;
      next_sample = 0.0f;

      const float frequency = fm.Next();
      const float saw_8_gain = saw_8_gain_modulation.Next();
      const float saw_4_gain = saw_4_gain_modulation.Next();
      const float saw_2_gain = saw_2_gain_modulation.Next();
      const float saw_1_gain = saw_1_gain_modulation.Next();

      phase += frequency;
      int next_segment = static_cast<int>(phase);
      if (next_segment != segment) {
        float discontinuity = 0.0f;
        if (next_segment == 8) {
          phase -= 8.0f;
          next_segment -= 8;
          discontinuity -= saw_8_gain;
        }
        if ((next_segment & 3) == 0) {
          discontinuity -= saw_4_gain;
        }
        if ((next_segment & 1) == 0) {
          discontinuity -= saw_2_gain;
        }
        discontinuity -= saw_1_gain;
        if (discontinuity != 0.0f) {
          float fraction = phase - static_cast<float>(next_segment);
          float t = fraction / frequency;
          this_sample += ThisBlepSample(t) * discontinuity;
          next_sample += NextBlepSample(t) * discontinuity;
        }
      }
      segment = next_segment;

      next_sample += (phase - 4.0f) * saw_8_gain * 0.125f;
      next_sample += (phase - float(segment & 4) - 2.0f) * saw_4_gain * 0.25f;
      next_sample += (phase - float(segment & 6) - 1.0f) * saw_2_gain * 0.5f;
      next_sample += (phase - float(segment & 7) - 0.5f) * saw_1_gain;
      *out++ += 2.0f * this_sample;
    }
    next_sample_ = next_sample;
    phase_ = phase;
    segment_ = segment;
  }

 private:
  // Oscillator state.
  float phase_;
  float next_sample_;
  int segment_;

  // For interpolation of parameters.
  float frequency_;
  float saw_8_gain_;
  float saw_4_gain_;
  float saw_2_gain_;
  float saw_1_gain_;
};

// ---------------------------------------------------------------------------
// WavetableOscillator<wavetable_size, num_waves>
// (plaits/dsp/oscillator/wavetable_oscillator.h)
//
// Integrated wavetable synthesis: interpolates a pre-integrated single-cycle
// waveform and differentiates the result, recovering the waveform with
// interpolation error spectrally shaped downward -- cheap anti-aliasing
// without mip-maps.
// ---------------------------------------------------------------------------

// The one constant needed from plaits/dsp/oscillator/oscillator.h.
static const float kChordOscMaxFrequency = 0.25f;

class Differentiator {
 public:
  Differentiator() { }
  ~Differentiator() { }

  void Init() {
    previous_ = 0.0f;
    lp_ = 0.0f;
  }

  float Process(float coefficient, float s) {
    ONE_POLE(lp_, s - previous_, coefficient);
    previous_ = s;
    return lp_;
  }
 private:
  float lp_;
  float previous_;
};

// Linear interpolation only -- InterpolateWaveHermite is dead code in the
// upstream file (WavetableOscillator::Render never calls it), so it is not
// vendored here. Not a template (unlike upstream) -- only ever called on
// int16_t tables in this fork, and this project has no other precedent for
// C++ templates, so a plain function avoids relying on unverified toolchain/
// object-loader behavior for template instantiation.
inline float InterpolateWave(
    const int16_t* table,
    int32_t index_integral,
    float index_fractional) {
  float a = static_cast<float>(table[index_integral]);
  float b = static_cast<float>(table[index_integral + 1]);
  float t = index_fractional;
  return a + (b - a) * t;
}

// Hardcoded for the chord engine's one configuration (128 samples/wave, 15
// waves, "approximate_scale" and "attenuate_high_frequencies" both true --
// upstream's template bool flags, always true for our one use site, so the
// upstream ternaries/branches on them are simplified away below).
// Not a template (unlike upstream's WavetableOscillator<size,waves,...>) --
// same rationale as InterpolateWave above.
static const uint32_t kChordWavetableSize = 128;
static const uint32_t kChordNumWaves = 15;

class WavetableOscillator {
 public:
  WavetableOscillator() { }
  ~WavetableOscillator() { }

  void Init() {
    phase_ = 0.0f;
    frequency_ = 0.0f;
    amplitude_ = 0.0f;
    waveform_ = 0.0f;
    lp_ = 0.0f;
    differentiator_.Init();
  }

  void Render(
      float frequency,
      float amplitude,
      float waveform,
      const int16_t* const* wavetable,
      float* out,
      uint32_t size) {
    CONSTRAIN(frequency, 0.0000001f, kChordOscMaxFrequency)

    amplitude *= 1.0f - 2.0f * frequency;
    amplitude *= 1.0f / (frequency * 131072.0f);

    ParameterInterpolator frequency_modulation(
        &frequency_,
        frequency,
        size);
    ParameterInterpolator amplitude_modulation(
        &amplitude_,
        amplitude,
        size);
    ParameterInterpolator waveform_modulation(
        &waveform_,
        waveform * float(kChordNumWaves - 1.0001f),
        size);

    float lp = lp_;
    float phase = phase_;
    while (size--) {
      const float f0 = frequency_modulation.Next();
      const float cutoff = std::min(float(kChordWavetableSize) * f0, 1.0f);

      phase += f0;
      if (phase >= 1.0f) {
        phase -= 1.0f;
      }

      const float waveform = waveform_modulation.Next();
      MAKE_INTEGRAL_FRACTIONAL(waveform);

      const float p = phase * float(kChordWavetableSize);
      MAKE_INTEGRAL_FRACTIONAL(p);

      const float x0 = InterpolateWave(
          wavetable[waveform_integral], p_integral, p_fractional);
      const float x1 = InterpolateWave(
          wavetable[waveform_integral + 1], p_integral, p_fractional);

      const float s = differentiator_.Process(
          cutoff,
          (x0 + (x1 - x0) * waveform_fractional) * 1.0f);
      ONE_POLE(lp, s, cutoff);
      *out++ += amplitude_modulation.Next() * lp;
    }
    lp_ = lp;
    phase_ = phase;
  }

 private:
  // Oscillator state.
  float phase_;

  // For interpolation of parameters.
  float frequency_;
  float amplitude_;
  float waveform_;
  float lp_;

  Differentiator differentiator_;
};

#endif  // PLAITS_CHORD_OSC_H_
