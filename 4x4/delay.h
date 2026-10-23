// delay.h — P3 processor (replaces the former wavefolder, 2026-10-19 on
// user request): a plain time delay, nothing more. Ranges 1-100ms.
//
// Follows the same base+depth model as P1/P2 (4x4-System-Overview.md §2/§3):
// the delay TIME has a randomized per-render base (held for the render),
// and a Contour-driven, depth-scaled offset moves it around that base,
// clamped to 1-100ms. "P3 depth" (Depth page) scales that modulation.
//
// MEMORY / OVERSAMPLING (user decision 2026-10-19): the ring buffer is
// sized for 100ms at the 48kHz BASE rate (kMaxSamples = 4800). The delay
// runs inside the oversampled feedback loop, so a requested delay of T ms
// costs T * internalRate/1000 samples; when the buffer can't hold that many
// (at higher oversampling, or a higher-than-48kHz module rate) the delay is
// clamped to what fits. Net effect, as agreed: max delay time is ~100ms at
// 1x and halves per oversampling step (50ms@2x, 25@4x, 12.5@8x). A pure
// delay doesn't alias, so there's no fidelity cost to it living in the
// oversampled domain — only this buffer-length scaling.
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace p3delay {

static const int kMaxSamples = 4800;   // 100ms @ 48kHz base rate
static const float kMinMs = 1.0f;
static const float kMaxMs = 100.0f;

// Pluggable random-base source, same interface/role as p1pitch/p2freqshift.
// Distinct additive constant so P3's base draw is decorrelated from the
// others at a given seed.
inline float defaultSeedDerivedBase( uint32_t seed, int renderIndex )
{
    uint32_t x = seed * 747796405u + (uint32_t)renderIndex * 2891336453u + 0x1F3A77C9u;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return (float)x / 4294967296.0f;   // [0,1)
}

class Processor
{
public:
    Processor() { reset(); }

    void reset()
    {
        memset( buf_, 0, sizeof( buf_ ) );
        writePos_ = 0;
        baseMs_ = 25.0f;                 // sensible default until onNewRender draws one
        randomBaseFn = defaultSeedDerivedBase;
    }

    // Draw a fresh base delay time for this render (held for its duration).
    void onNewRender( uint32_t seed, int renderIndex )
    {
        float r = randomBaseFn ? randomBaseFn( seed, renderIndex ) : 0.25f;
        baseMs_ = kMinMs + r * ( kMaxMs - kMinMs );   // 1..100ms
        // Note: the buffer is NOT cleared here — onNewRender in the feedback
        // network already cold-resets via reset() for state continuity.
    }

    // depthMs: Contour-driven, depth-scaled offset (both signs). The final
    // delay = clamp(base + offset, 1, 100) ms, then clamped to the buffer.
    float processSample( float x, float depthMs, float sampleRate )
    {
        float delayMs = baseMs_ + depthMs;
        if ( delayMs < kMinMs ) delayMs = kMinMs;
        if ( delayMs > kMaxMs ) delayMs = kMaxMs;

        int d = (int)( delayMs * sampleRate / 1000.0f + 0.5f );
        if ( d < 1 ) d = 1;
        if ( d > kMaxSamples - 1 ) d = kMaxSamples - 1;   // buffer limit (scales with rate)

        buf_[writePos_] = x;
        int rp = writePos_ - d;
        if ( rp < 0 ) rp += kMaxSamples;
        float y = buf_[rp];

        ++writePos_;
        if ( writePos_ >= kMaxSamples ) writePos_ = 0;
        return y;
    }

    float (*randomBaseFn)( uint32_t seed, int renderIndex );

private:
    float buf_[kMaxSamples];
    int   writePos_;
    float baseMs_;
};

} // namespace p3delay
