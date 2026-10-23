// kit_buffer.h — kit audio storage: 12-bit packed, 32kHz, mono, double
// buffered. Implementation Plan Step 13, per 4x4-System-Overview.md §8.
//
// Sample count derivation: §8's original estimate (per-category formula:
// ~25KB FFT scratch + ~44KB Contour + ~1KB matrix/filter state, leaving
// ≈221.46875KB/buffer) gave kNumSamples=151188 at first, but Step 14's
// ARM cross-compile of the FULL assembled Renderer (ContourDestinations +
// FeedbackNetwork + this buffer together, not the separate category
// estimates) measured 534,192 real bytes — about 9.67KB OVER the locked
// 512KB DRAM budget (§8). The per-category estimates weren't precise
// enough (struct padding/alignment and the real combined footprint don't
// sum the same way the rough per-category math assumed). A first
// correction using the REAL measured sizes of ContourDestinations
// (45,240B) and FeedbackNetwork (35,352B) landed EXACTLY at 524,288 bytes
// (512KB) on ARM — zero margin, which alignment padding in a slightly
// different build could tip over, and which leaves nothing for Step 15's
// still-unknown additions. Backed off with a genuine 2KB safety margin:
// kNumSamples = 147204 at 220,806 bytes/buffer (4.600125s).
//
// **REGRESSION FIX 2026-10-20 — kNumSamples 147204 -> 141000.** The 4.60s
// size left only ~2-4KB of margin, and the earlier budgeting only ever
// checked `sizeof(Engine)` against the 512KB pool — it NEVER accounted for
// the plugin's own static `.bss` (~8KB, dominated by fft.h's g_twiddle
// table) which the firmware loader must ALSO place in RAM. After the P3
// delay buffer (+19KB) landed, the real total became: static bss 8,196 B +
// req.dram Engine 519,936 B = 528,132 B, which OVERRUNS the 512KB
// (524,288 B) pool by ~3.8KB. That is the most likely cause of the plugin
// going silent on hardware (desktop has separate bss/heap and abundant RAM,
// so it never showed — the last build that fit, "step 15", was 522,292 B
// total and worked on the module). Trimmed the kit so static+dynamic sits
// comfortably under 512KB with ~14KB margin; the static_assert in 4x4.cpp
// now reserves for the static footprint too, so this can't recur.
//
// Per §6, "Length is locked to always equal the kit's own maximum
// duration" — this header's kDurationSec is that single source of truth.
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>

namespace kitbuffer {

static const int kSampleRateHz = 32000;          // locked, §8
static const int kNumSamples = 141000;            // even; trimmed for DRAM margin (see above)
static const int kBytesPerBuffer = kNumSamples / 2 * 3;   // 211,500 bytes
static const float kDurationSec = (float)kNumSamples / (float)kSampleRateHz;   // 4.40625s

// Full-scale quantization range: the hardware's absolute output ceiling
// (CLAUDE.md: max output range +-11V), matching what Reg (§1) already
// guarantees the rendered signal never exceeds.
static const float kFullScaleVolts = 11.0f;

// Packs two signed 12-bit samples (range [-2048,2047]) into 3 bytes:
// byte0 = a[11:4], byte1 = (a[3:0]<<4)|(b[11:8] as 4 bits), byte2 = b[7:0].
inline void pack12( int16_t a, int16_t b, uint8_t* out3 )
{
    uint16_t ua = (uint16_t)( a & 0x0FFF );
    uint16_t ub = (uint16_t)( b & 0x0FFF );
    out3[0] = (uint8_t)( ua >> 4 );
    out3[1] = (uint8_t)( ( ( ua & 0x0F ) << 4 ) | ( ub >> 8 ) );
    out3[2] = (uint8_t)( ub & 0xFF );
}

inline void unpack12( const uint8_t* in3, int16_t& a, int16_t& b )
{
    uint16_t ua = (uint16_t)( ( in3[0] << 4 ) | ( in3[1] >> 4 ) );
    uint16_t ub = (uint16_t)( ( ( in3[1] & 0x0F ) << 8 ) | in3[2] );
    // Sign-extend from 12 bits.
    a = (int16_t)( ua << 4 ) >> 4;
    b = (int16_t)( ub << 4 ) >> 4;
}

// One kit's worth of 12-bit packed mono audio, fixed-size, no allocation.
class KitBuffer
{
public:
    void clear() { memset( bytes_, 0, sizeof( bytes_ ) ); }

    // Writes sample i (volts, +-kFullScaleVolts) into the packed store.
    // Order-independent within a pair (0,1), (2,3), ...: each write reads
    // back whatever currently occupies the OTHER slot in its shared
    // 3-byte group (zero, if that slot hasn't been written yet in a
    // freshly-cleared buffer) and preserves it, overwriting only its own
    // slot. kNumSamples is kept even (no incomplete trailing pair) so
    // every index always has a valid partner within the buffer.
    void write( int index, float volts )
    {
        int16_t level = voltsToLevel( volts );
        int pairIndex = index / 2;
        uint8_t* group = bytes_ + pairIndex * 3;
        int16_t a, b;
        unpack12( group, a, b );
        if ( ( index & 1 ) == 0 ) pack12( level, b, group );
        else pack12( a, level, group );
    }

    float read( int index ) const
    {
        int pairIndex = index / 2;
        const uint8_t* group = bytes_ + pairIndex * 3;
        int16_t a, b;
        unpack12( group, a, b );
        return levelToVolts( ( index & 1 ) == 0 ? a : b );
    }

    static int16_t voltsToLevel( float volts )
    {
        float v = volts;
        if ( v > kFullScaleVolts ) v = kFullScaleVolts;
        if ( v < -kFullScaleVolts ) v = -kFullScaleVolts;
        int32_t level = (int32_t)floorf( v / kFullScaleVolts * 2047.0f + 0.5f );
        if ( level > 2047 ) level = 2047;
        if ( level < -2048 ) level = -2048;
        return (int16_t)level;
    }

    static float levelToVolts( int16_t level )
    {
        return (float)level / 2047.0f * kFullScaleVolts;
    }

private:
    uint8_t bytes_[kBytesPerBuffer];
};

// Double-buffered kit store (§6): a current (playable) and a next (being
// rendered) buffer, both fully resident. The renderer only ever writes
// the inactive buffer; swap() makes it current in one indivisible step
// (a single index flip), only once a render is fully complete -- the
// player must never observe a partially-rendered buffer.
class DoubleBufferedKit
{
public:
    DoubleBufferedKit() : activeIndex_( 0 )
    {
        buffers_[0].clear();
        buffers_[1].clear();
    }

    const KitBuffer& current() const { return buffers_[activeIndex_]; }
    KitBuffer& inactive() { return buffers_[1 - activeIndex_]; }
    int activeIndex() const { return activeIndex_; }   // for syncing per-buffer side data (e.g. period maps)

    // Atomic in the sense that matters here: a single write to one
    // integer. This is single-threaded cooperative code (no OS, no
    // preemption inside step() per CLAUDE.md), so the player can only ever
    // observe activeIndex_ either fully before or fully after this call,
    // never a torn/partial state.
    void swap() { activeIndex_ = 1 - activeIndex_; }

private:
    KitBuffer buffers_[2];
    int activeIndex_;
};

} // namespace kitbuffer
