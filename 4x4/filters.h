// filters.h — per-channel HPF/LPF band-limiting pair, and the Reg
// regulator chain (DC block -> AGC -> soft clip). Implementation Plan
// Step 11, per 4x4-System-Overview.md §1.
//
// HPF/LPF cutoffs are plain scalar inputs per call for now (Step 11 scope)
// -- Step 12 replaces them with Contour-driven values, same pattern as
// P3's fold/bias and P4's bits/divisor.
//
// OPEN DESIGN QUESTION, FLAGGED PER THE PLAN (not a silent default): §10
// leaves peak-vs-RMS level detection for the AGC explicitly open, asking
// for a listening-pass decision rather than a silent pick. Peak detection
// is implemented below because the plan names it "the simpler default,"
// but this has NOT been confirmed by ear on real self-oscillating content
// -- RMS detection would react less to single-sample transients and more
// to sustained loudness, which could matter a lot for how "pumpy" the AGC
// feels on this specific signal path. Revisit after Step 16's on-device
// listening. See 4x4-System-Overview.md §10.
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11.
#pragma once
#include <cmath>

namespace filters {

// One-pole lowpass (6dB/octave), the cheapest standard IIR lowpass.
class OnePoleLowpass
{
public:
    OnePoleLowpass() : y_( 0.0f ) {}
    void reset() { y_ = 0.0f; }
    float process( float x, float cutoffHz, float sampleRate )
    {
        float coeff = expf( -2.0f * 3.14159265f * cutoffHz / sampleRate );
        y_ = coeff * y_ + ( 1.0f - coeff ) * x;
        return y_;
    }
private:
    float y_;
};

// Resonant bandpass biquad — the per-channel pre-processor filter
// (replaces the former one-pole HPF+LPF pair, 2026-10-20 user request).
// RBJ-cookbook constant-0dB-peak-gain bandpass: 0dB at the center frequency,
// attenuating away from it; narrow bandwidth = high resonance, wide = nearly
// wideband. No gain parameter — only the band passes. Bandwidth is specified
// in SEMITONES (1..96 = 1 semitone .. 8 octaves) and converted to the
// cookbook's octave form internally.
//
// Coefficients are computed only in setParams() (the filter params are static
// per render), NOT per sample, so process() is a cheap 5-mul biquad with no
// transcendentals. reset() installs passthrough coeffs so an unconfigured
// instance passes signal unchanged.
class Bandpass
{
public:
    Bandpass() { reset(); }

    void reset()
    {
        x1_ = x2_ = y1_ = y2_ = 0.0f;
        b0_ = 1.0f; b1_ = 0.0f; b2_ = 0.0f; a1_ = 0.0f; a2_ = 0.0f;   // passthrough
    }

    void setParams( float freqHz, float bwSemitones, float sampleRate )
    {
        const float kPi = 3.14159265358979f;
        // Keep the center well inside (0, Nyquist) for a stable biquad.
        float nyq = 0.5f * sampleRate;
        if ( freqHz < 1.0f ) freqHz = 1.0f;
        if ( freqHz > nyq * 0.99f ) freqHz = nyq * 0.99f;
        if ( bwSemitones < 1.0f ) bwSemitones = 1.0f;
        if ( bwSemitones > 96.0f ) bwSemitones = 96.0f;

        float w0 = 2.0f * kPi * freqHz / sampleRate;
        float sinw = sinf( w0 );
        float cosw = cosf( w0 );
        float bwOct = bwSemitones / 12.0f;
        // RBJ bandwidth->alpha (constant 0dB peak gain BPF).
        float alpha = sinw * sinhf( 0.34657359f * bwOct * ( w0 / ( sinw != 0.0f ? sinw : 1e-9f ) ) );
        float a0 = 1.0f + alpha;
        b0_ =  alpha / a0;
        b1_ =  0.0f;
        b2_ = -alpha / a0;
        a1_ = ( -2.0f * cosw ) / a0;
        a2_ = ( 1.0f - alpha ) / a0;
    }

    float process( float x )
    {
        float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
        x2_ = x1_; x1_ = x;
        y2_ = y1_; y1_ = y;
        return y;
    }

private:
    float b0_, b1_, b2_, a1_, a2_;
    float x1_, x2_, y1_, y2_;
};

// Anti-aliasing lowpass used before the renderer decimates its oversampled
// mono mix down to the 32kHz kit (renderer.h / 4x4-System-Overview.md §7).
// A cascade of 4 one-poles (~24 dB/oct) at a fixed cutoff just below the
// 32kHz kit's 16kHz Nyquist, run at the (oversampled) internal rate. This is
// a deliberately simple, gentle AA stage — NOT a brickwall filter — chosen
// for its zero extra memory and trivial cost; it removes the bulk of the
// energy that would otherwise fold into the stored kit. Could be upgraded to
// a steeper polyphase FIR later if the residual fold proves audible.
class AntiAliasLowpass
{
public:
    enum { kStages = 4 };

    void reset() { for ( int i = 0; i < kStages; ++i ) lp_[i].reset(); }
    float process( float x, float sampleRate )
    {
        const float kCutoffHz = 13000.0f;   // under the 16kHz kit Nyquist
        float y = x;
        for ( int i = 0; i < kStages; ++i ) y = lp_[i].process( y, kCutoffHz, sampleRate );
        return y;
    }
private:
    OnePoleLowpass lp_[kStages];
};

// One-pole highpass (6dB/octave). TWO earlier attempts were checked
// numerically and discarded (test_filters.cpp caught both): (1) the
// "complementary" form x - lowpass(x) measured ~0.66 instead of 0.707 at
// the nominal cutoff and only ~0.93 pass-through at 10x cutoff instead of
// approaching 1.0; (2) algebraically that form turned out to be identical
// to the unnormalized recursion y[n] = a*(y[n-1]+x[n]-x[n-1]) -- same
// measured failure, because that recursion's own passband ceiling at
// Nyquist is 2a/(1+a), not 1.0, for any a < 1 (verified by direct
// transfer-function evaluation at z=-1). The fix is a normalizing gain
// factor (1+a)/2 on the difference term, which forces H(z=-1) = 1 exactly
// for any a -- the standard, correct one-pole highpass form.
class OnePoleHighpass
{
public:
    OnePoleHighpass() : x1_( 0.0f ), y1_( 0.0f ) {}
    void reset() { x1_ = y1_ = 0.0f; }
    float process( float x, float cutoffHz, float sampleRate )
    {
        float a = expf( -2.0f * 3.14159265f * cutoffHz / sampleRate );
        float y = ( 1.0f + a ) * 0.5f * ( x - x1_ ) + a * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }
private:
    float x1_, y1_;
};

// DC blocker: a dedicated one-pole highpass fixed at a very low cutoff,
// conceptually distinct from the per-channel band-limiting HPF above
// (removes persistent DC/near-DC only, not meant to shape the audible
// band). Classic form: y[n] = x[n] - x[n-1] + R*y[n-1].
class DcBlocker
{
public:
    DcBlocker() : x1_( 0.0f ), y1_( 0.0f ) {}
    void reset() { x1_ = y1_ = 0.0f; }
    float process( float x )
    {
        const float kR = 0.995f;   // cutoff well below audio band at any reasonable sample rate
        float y = x - x1_ + kR * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }
private:
    float x1_, y1_;
};

// AGC: peak envelope follower (see header note on peak-vs-RMS) driving
// gain toward a single target level at the midpoint of the locked
// -18dB..-6dB window (relative to the +-11V hardware ceiling) -- -12dB,
// ~2.763V. Attack/release within the locked 2-10ms / 100-500ms ranges.
class Agc
{
public:
    Agc() : envelope_( 0.0f ) {}
    void reset() { envelope_ = 0.0f; }

    float process( float x, float sampleRate )
    {
        const float kAttackSec = 0.005f;    // 5ms, within locked 2-10ms range
        const float kReleaseSec = 0.25f;    // 250ms, within locked 100-500ms range
        const float kTargetVolts = 2.763f;  // -12dB relative to 11V ceiling, midpoint of -18..-6dB

        float absX = fabsf( x );
        float coeff = ( absX > envelope_ )
            ? ( 1.0f - expf( -1.0f / ( kAttackSec * sampleRate ) ) )
            : ( 1.0f - expf( -1.0f / ( kReleaseSec * sampleRate ) ) );
        envelope_ += ( absX - envelope_ ) * coeff;

        const float kMinEnvelope = 1e-4f;   // avoid divide-by-near-zero on silence
        float gain = kTargetVolts / ( envelope_ > kMinEnvelope ? envelope_ : kMinEnvelope );
        return x * gain;
    }
private:
    float envelope_;
};

// Soft clip: tanh, scaled so output is bounded to +-11V (the hardware's
// absolute output ceiling) for ANY input, never a hard clip. Near-identity
// for |x| << 11V; progressively compresses as x approaches/exceeds it.
inline float softClip( float x )
{
    const float kCeiling = 11.0f;
    return kCeiling * tanhf( x / kCeiling );
}

// Full Reg chain: DC block -> AGC -> soft clip.
class Reg
{
public:
    void reset() { dc_.reset(); agc_.reset(); }
    float process( float x, float sampleRate )
    {
        float y = dc_.process( x );
        y = agc_.process( y, sampleRate );
        y = softClip( y );
        return y;
    }
private:
    DcBlocker dc_;
    Agc agc_;
};

} // namespace filters
