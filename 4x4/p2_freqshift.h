// p2_freqshift.h — P2 frequency-shift processor.
// Implementation Plan Step 7. Standalone processor unit, not yet wired into
// the full plugin. Uses the allpass coefficients derived and verified in
// Step 6 (see design_hilbert.cpp and 4x4-System-Overview.md §3).
//
// Structure (per §3): two allpass chains build an approximate analytic
// signal from the real input, a rotating phasor at the shift frequency
// performs the complex multiply, and the real part is extracted. Entirely
// sample-by-sample, no block latency, unlike P1.
//
// Parameter spec, settled here per the plan's Step 7 prompt (§3/§10 left
// this open): **base + depth, same treatment as P1** — a fresh random base
// shift drawn at the start of each render (bipolar, within the full
// range), held fixed for that render, plus a real performer-facing depth
// parameter that offsets around it. This mirrors P1's rationale exactly
// (4x4-System-Overview.md §2/§9): a frequency shifter benefits from the
// same "every render sounds like a different instrument" character pitch
// shift gets, rather than starting silent/neutral like P4's curve-only
// controls. Unit: Hz, bipolar. Range: ±2000Hz for the combined
// base+depth total — a deliberately wide, dramatic range (true frequency
// shifting turns harmonic content inharmonic within a few tens of Hz, so
// even this is already extreme over most of its range), chosen to be
// comparably "wide but bounded" to P1's ±24 semitones, and to stay
// comfortably inside the allpass design's verified <1-degree-accurate
// range (~16Hz-23.5kHz per Step 6) — the SHIFT amount, not the input
// frequency, is what's bounded here, so ±2000Hz never asks the Hilbert
// approximation itself to operate outside its validated range.
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11.
#pragma once
#include <cmath>
#include <cstdint>

namespace p2freqshift {

// First-order allpass section operating on the 2-sample-delay variable
// Z = z^-2: H(Z) = (a + Z) / (1 + a*Z), implemented as the standard
// two-register transposed form: y[n] = a*(x[n] - y[n-2]) + x[n-2].
class AllpassSection
{
public:
    AllpassSection( float a ) : a_( a ), x1_( 0.0f ), x2_( 0.0f ), y1_( 0.0f ), y2_( 0.0f ) {}

    float process( float x0 )
    {
        float y0 = a_ * ( x0 - y2_ ) + x2_;
        x2_ = x1_; x1_ = x0;
        y2_ = y1_; y1_ = y0;
        return y0;
    }

    void reset() { x1_ = x2_ = y1_ = y2_ = 0.0f; }

private:
    float a_;
    float x1_, x2_, y1_, y2_;
};

// Pluggable random-base source, same interface as p1pitch (see
// p1_pitchshift.h and 4x4-System-Overview.md §2/§10's open question on
// seed-derived vs. entropy-based randomization).
inline float defaultSeedDerivedBase( uint32_t seed, int renderIndex )
{
    uint32_t x = seed * 747796405u + (uint32_t)renderIndex * 2891336453u + 0x2C1B3C6Du;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return (float)x / 4294967296.0f;   // [0,1)
}

static const int kNumSections = 6;

// Coefficients derived in Step 6 (design_hilbert.cpp) via nonlinear
// least-squares against the actual 90-degree phase-difference criterion,
// not recalled/fabricated. See 4x4-System-Overview.md §3 for the
// verification numbers.
static const float kChainACoeffs[kNumSections] = {
    -0.9983674986f, -0.2712161970f, -0.9887756210f, -0.9625251036f, -0.6730934176f, -0.8841466094f
};
static const float kChainBCoeffs[kNumSections] = {
    -0.8019822326f, -0.4900702536f, -0.9336325065f, -0.0787602760f, -0.9945280131f, -0.9791624864f
};

class Processor
{
public:
    Processor() :
        a0( kChainACoeffs[0] ), a1( kChainACoeffs[1] ), a2( kChainACoeffs[2] ),
        a3( kChainACoeffs[3] ), a4( kChainACoeffs[4] ), a5( kChainACoeffs[5] ),
        b0( kChainBCoeffs[0] ), b1( kChainBCoeffs[1] ), b2( kChainBCoeffs[2] ),
        b3( kChainBCoeffs[3] ), b4( kChainBCoeffs[4] ), b5( kChainBCoeffs[5] )
    {
        reset();
    }

    void reset()
    {
        a0.reset(); a1.reset(); a2.reset(); a3.reset(); a4.reset(); a5.reset();
        b0.reset(); b1.reset(); b2.reset(); b3.reset(); b4.reset(); b5.reset();
        chainADelay1_ = 0.0f;
        phase_ = 0.0f;
        baseHz_ = 0.0f;
        randomBaseFn = defaultSeedDerivedBase;
    }

    // Call once at the start of each new render (kit generation).
    void onNewRender( uint32_t seed, int renderIndex )
    {
        float r = randomBaseFn ? randomBaseFn( seed, renderIndex ) : 0.5f;
        baseHz_ = ( r * 2.0f - 1.0f ) * 2000.0f;
        phase_ = 0.0f;
    }

    // depthHz: real-valued performer offset around the randomized base.
    // Combined total is clamped to +/-2000Hz (see file header).
    float processSample( float x, float depthHz, float sampleRate )
    {
        float shiftHz = baseHz_ + depthHz;
        if ( shiftHz > 2000.0f ) shiftHz = 2000.0f;
        if ( shiftHz < -2000.0f ) shiftHz = -2000.0f;

        float aOut = a0.process( x );
        aOut = a1.process( aOut );
        aOut = a2.process( aOut );
        aOut = a3.process( aOut );
        aOut = a4.process( aOut );
        aOut = a5.process( aOut );

        float bOut = b0.process( x );
        bOut = b1.process( bOut );
        bOut = b2.process( bOut );
        bOut = b3.process( bOut );
        bOut = b4.process( bOut );
        bOut = b5.process( bOut );

        float aDelayed = chainADelay1_;
        chainADelay1_ = aOut;

        const float kTwoPi = 6.283185307f;
        const float kPi = 3.14159265f;
        phase_ += kTwoPi * shiftHz / sampleRate;
        phase_ = phase_ - kTwoPi * floorf( ( phase_ + kPi ) / kTwoPi );

        return aDelayed * cosf( phase_ ) + bOut * sinf( phase_ );
    }

    float (*randomBaseFn)( uint32_t seed, int renderIndex );

private:
    AllpassSection a0, a1, a2, a3, a4, a5;
    AllpassSection b0, b1, b2, b3, b4, b5;
    float chainADelay1_;
    float phase_;
    float baseHz_;
};

} // namespace p2freqshift
