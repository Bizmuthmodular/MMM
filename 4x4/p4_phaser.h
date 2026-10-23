// p4_phaser.h — P4 12-stage allpass phaser processor.
//
// Replaces the former bitcrusher (omitted 2026-10-21 per user request:
// "add a 12 stage phaser instead ... just use its phase shifter with 12
// stages as a processor instead of the crusher"). This is the phase-SHIFTER
// core only — NO internal feedback, NO LFO/trigger of its own. The frequency
// sweep that a phaser's LFO would normally supply comes instead from this
// plugin's Contour system (the P4 center-frequency destination, depth-scaled),
// exactly as every other processor here is modulated (§2-§5/§6).
//
// Structure: 12 cascaded first-order allpass sections, all sharing one center
// frequency fc. Each section is
//     H(z) = (a + z^-1) / (1 + a z^-1)
//   => y[n] = a*x[n] + x[n-1] - a*y[n-1]
//   with a = (t - 1)/(t + 1),  t = tan(pi*fc/fs),  a in (-1, 1) for
//   0 < fc < fs/2. This is the coefficient that places the section's -90deg
//   quadrature point exactly at fc: solving Re{H(e^jwc)}=0 gives
//   cos wc = -2a/(1+a^2) => a = (t-1)/(t+1). (The inverted sign (1-t)/(1+t)
//   mirrors fc around fs/4 and sweeps the comb backwards — a bug caught by the
//   sweep-direction test.) The section (and the whole cascade) is
//   unconditionally stable at every center frequency. Each section is magnitude-flat
//   (an allpass); the audible phaser NOTCHES appear only in the dry+wet MIX:
//     out = 0.5 * (dry + wet),  wet = 12-stage allpass output.
//   The total allpass phase runs 0 -> -12*pi across 0 -> fs/2, so the mix
//   magnitude is |cos(6*phi_single)| — exactly SIX notches that slide with fc.
//
// Control model matches P1/P2/P3: a RANDOMIZED PER-RENDER BASE center
// frequency plus a Contour-driven, depth-scaled signed OFFSET in Hz. "P4
// depth" (Depth page) scales the sweep. processSample() takes the signed Hz
// offset and the sample rate (the coefficient depends on fs, which changes
// with oversampling, §7).
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11.
#pragma once
#include <cmath>
#include <cstdint>

namespace p4phaser {

static const int   kStages      = 12;
static const float kPi          = 3.14159265358979323846f;
static const float kBaseFreqMin = 300.0f;    // randomized base center range (Hz)
static const float kBaseFreqMax = 1800.0f;
static const float kFreqFloor   = 40.0f;     // absolute clamp for fc after the offset
static const float kFreqCeil    = 8000.0f;   // (also kept below 0.45*fs at runtime)

// Pluggable random-base source; distinct additive constant so P4's base draw
// is decorrelated from P1/P2/P3 at a given seed.
inline float defaultSeedDerivedBase( uint32_t seed, int renderIndex )
{
    uint32_t x = seed * 747796405u + (uint32_t)renderIndex * 2891336453u + 0x1B873593u;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return (float)x / 4294967296.0f;   // [0,1)
}

class Processor
{
public:
    Processor() { reset(); }

    void reset()
    {
        for ( int i = 0; i < kStages; ++i ) { apX1_[i] = 0.0f; apY1_[i] = 0.0f; }
        baseFreq_    = 600.0f;   // sensible default until onNewRender draws one
        randomBaseFn = defaultSeedDerivedBase;
    }

    // Draw a fresh base center frequency for this render.
    void onNewRender( uint32_t seed, int renderIndex )
    {
        float r = randomBaseFn ? randomBaseFn( seed, renderIndex ) : 0.4f;
        baseFreq_ = kBaseFreqMin + r * ( kBaseFreqMax - kBaseFreqMin );
    }

    // freqOffset: Contour-driven, depth-scaled signed Hz offset added to the
    // randomized base. sampleRate is the internal (possibly oversampled) rate.
    float processSample( float x, float freqOffset, float sampleRate )
    {
        float fc = baseFreq_ + freqOffset;
        float ceil = 0.45f * sampleRate;
        if ( ceil > kFreqCeil ) ceil = kFreqCeil;
        if ( fc < kFreqFloor ) fc = kFreqFloor;
        if ( fc > ceil )       fc = ceil;

        float t = tanf( kPi * fc / sampleRate );
        float a = ( t - 1.0f ) / ( t + 1.0f );   // -90deg at fc; in (-1,1) => stable

        float w = x;
        for ( int i = 0; i < kStages; ++i )
        {
            float y = a * w + apX1_[i] - a * apY1_[i];
            apX1_[i] = w;
            // Denormal flush on the recursive state: with zero input a stable
            // allpass decays geometrically into denormal range, which the
            // Cortex-M7 FPU handles far slower than desktop x86 (a CPU-spike /
            // mute risk inside the feedback loop). Flushing sub-audible values
            // to 0 is free and audibly a no-op (mirrors feedback_network.h).
            if ( fabsf( y ) < 1e-20f ) y = 0.0f;
            apY1_[i] = y;
            w = y;
        }
        // Classic phaser blend: dry + phase-shifted, 0.5 keeps the level sane.
        return 0.5f * ( x + w );
    }

    float (*randomBaseFn)( uint32_t seed, int renderIndex );

private:
    float apX1_[kStages];
    float apY1_[kStages];
    float baseFreq_;
};

} // namespace p4phaser
