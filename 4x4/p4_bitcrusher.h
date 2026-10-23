// p4_bitcrusher.h — P4 bitcrusher / sample-rate-reducer processor.
// Per 4x4-System-Overview.md §5: zero-order-hold sample-rate reduction
// feeding a round-to-nearest-step bit-depth quantizer. No anti-aliasing, no
// dither — aliasing/quantization artifacts are the intended lo-fi character
// (optionally cleaned by the global Oversample control, §7).
//
// Control model updated 2026-10-19 to match P1/P2/P3 (user request): both
// the bit depth and the rate divisor have a RANDOMIZED PER-RENDER BASE,
// and a Contour-driven, depth-scaled OFFSET moves each around its base.
// "P4 depth" (Depth page) scales that modulation. So processSample() now
// takes signed offsets, not absolute values, and combines them with the
// internally-held bases (exactly as P1 adds a Contour depth onto its
// randomized base pitch).
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11.
#pragma once
#include <cmath>
#include <cstdint>

namespace p4bitcrusher {

static const float kBitsMin = 1.0f, kBitsMax = 16.0f;
static const float kDivMin = 1.0f, kDivMax = 64.0f;

// Pluggable random-base source; distinct additive constant so P4's base
// draws are decorrelated from P1/P2/P3 at a given seed.
inline float defaultSeedDerivedBase( uint32_t seed, int renderIndex, int which )
{
    uint32_t x = seed * 747796405u + (uint32_t)renderIndex * 2891336453u
               + (uint32_t)which * 668265263u + 0x5D7A1B4Fu;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return (float)x / 4294967296.0f;   // [0,1)
}

class Processor
{
public:
    Processor() { reset(); }

    void reset()
    {
        held_ = 0.0f;
        holdCounter_ = 0;
        baseBits_ = 16.0f;     // sensible defaults until onNewRender draws them
        baseDivisor_ = 1.0f;
        randomBaseFn = defaultSeedDerivedBase;
    }

    // Draw fresh base bit-depth and rate-divisor for this render.
    void onNewRender( uint32_t seed, int renderIndex )
    {
        float rb = randomBaseFn ? randomBaseFn( seed, renderIndex, 0 ) : 0.5f;
        float rd = randomBaseFn ? randomBaseFn( seed, renderIndex, 1 ) : 0.0f;
        baseBits_   = kBitsMin + rb * ( kBitsMax - kBitsMin );
        baseDivisor_ = kDivMin  + rd * ( kDivMax  - kDivMin );
    }

    // divisorOffset / bitsOffset: Contour-driven, depth-scaled offsets. Each
    // is added to the randomized base and clamped to range. Full scale for
    // quantization is the ±5V audio convention (CLAUDE.md).
    float processSample( float x, float divisorOffset, float bitsOffset )
    {
        float divf = baseDivisor_ + divisorOffset;
        if ( divf < kDivMin ) divf = kDivMin;
        if ( divf > kDivMax ) divf = kDivMax;
        int divisor = (int)( divf + 0.5f );

        float bitsf = baseBits_ + bitsOffset;
        if ( bitsf < kBitsMin ) bitsf = kBitsMin;
        if ( bitsf > kBitsMax ) bitsf = kBitsMax;
        int bits = (int)( bitsf + 0.5f );

        if ( holdCounter_ == 0 )
            held_ = x;
        ++holdCounter_;
        if ( holdCounter_ >= divisor )
            holdCounter_ = 0;

        const float kFullScale = 5.0f;
        float levels = (float)( ( 1u << bits ) - 1u );
        float step = kFullScale / levels;
        return roundf( held_ / step ) * step;
    }

    float (*randomBaseFn)( uint32_t seed, int renderIndex, int which );

private:
    float held_;
    int   holdCounter_;
    float baseBits_;
    float baseDivisor_;
};

} // namespace p4bitcrusher
