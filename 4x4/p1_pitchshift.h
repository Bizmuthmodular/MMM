// p1_pitchshift.h — P1 pitch-shift processor.
//
// FULL-SPECTRUM REWRITE 2026-10-22 (user request, pitch-shifter-swap-omit.md).
// The previous P1 was a monophonic single-partial tracker (it kept only the
// loudest bin each frame and zeroed the rest), which left no spectrum for the
// requested BlockSize/Swaps/SwapSize/Omit spectral manipulation to act on.
// This version processes ALL bins, implementing the §2 "Ocean" design
// (Juillerat 2010) that was originally intended: a phase-locked BIN-SHIFT with
// NO atan2 / no per-bin phase tracking.
//
// How the shift works (and why it needs no atan2):
//   * Forward FFT of the Hann-windowed 1024-sample block (hop 256, 4x overlap).
//   * Each source bin k's complex content is moved to output bin
//     dst = round(k_eff * ratio) (k_eff after the swap below), relocating the
//     spectral envelope by `ratio` => pitch shift. Collisions sum; gaps stay 0.
//   * To keep a relocated partial phase-coherent across overlapping frames, its
//     content is rotated by exp(j*2pi*Δ*hop/N) per hop, Δ = dst-k the NET
//     displacement from the analysis bin to the output bin. A partial analysed
//     at bin k advances 2pi*k*hop/N per hop; adding the rotation makes it
//     advance 2pi*dst*hop/N — exactly bin dst's own rate, so overlap-add
//     reconstructs it coherently. For N=1024, hop=256 (hop/N = 1/4) the per-hop
//     increment is 2pi*Δ/4, so at frame m the accumulated rotation is
//     exp(j*2pi*Δ*m/4) = j^(Δ*m) — one of {1, j, -1, -j} indexed by (Δ*m)&3.
//     Integer Δ makes this EXACT (no quantisation), and it costs a couple of
//     sign swaps per bin — far cheaper than 511 atan2 calls/hop, so it does not
//     re-introduce the "render bleed" CPU problem.
//
// Spectral manipulation (maps fixed for the whole render, generated once at
// onNewRender from the per-render seed => reproducible):
//   * BlockSize B in {1,2,4,8,16,32,64}: swapping/muting works on whole blocks
//     of B contiguous operable bins. Operable range = bins 1..kOperable
//     (= kNumBins-2 = 511); DC (0) and Nyquist (512) are never touched;
//     leftover bins above the last whole block pass through.
//   * Omit (0..100% of numBlocks): mute that many random blocks, with a
//     raised-cosine edge ramp R=min(B/4,4) to avoid brick-wall ringing.
//   * Swaps (0..100% of numBlocks/2) disjoint block PAIRS exchange places,
//     SwapSize (0..100% of numBlocks/2) = max circular block distance (wraps).
//   * ORDER per frame (user choice "swap before shift"): Omit source bins ->
//     Swap blocks -> Shift. The phase rotation uses the net displacement
//     dst - k regardless of order.
//
// At Swaps=0 and Omit=0 the maps are identity/unity, so the shifter runs clean
// full-spectrum (this is the new P1 baseline; the old single-partial sound is
// intentionally gone).
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11.
//
// REQUIRES fft::initTwiddles() TO HAVE BEEN CALLED ONCE before any Processor is
// used (fft::transform() internally). Forgetting it fails silently (twiddles
// all-zero => every output 0.0). The factory initialise() does this.
#pragma once
#include "fft.h"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace p1pitch {

static const int kBlockSize = 1024;
static const int kHopSize   = 256;                  // 4x overlap
static const int kNumBins   = kBlockSize / 2 + 1;   // 513 (0..512)
static const int kOperable  = kNumBins - 2;         // 511 swappable/mutable bins (1..511)
static const int kRotPeriod = kBlockSize / kHopSize; // 4 (hop/N = 1/4) -> j^(Δm), (Δm)&3

// Pluggable random-base source (base pitch draw, §2).
inline float defaultSeedDerivedBase( uint32_t seed, int renderIndex )
{
    uint32_t x = seed * 747796405u + (uint32_t)renderIndex * 2891336453u + 0x91E10DA5u;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return (float)x / 4294967296.0f;   // [0,1)
}

class Processor
{
public:
    Processor() { reset(); }

    void reset()
    {
        memset( inBuf_, 0, sizeof( inBuf_ ) );
        memset( outBuf_, 0, sizeof( outBuf_ ) );
        memset( hopIn_, 0, sizeof( hopIn_ ) );
        memset( outReady_, 0, sizeof( outReady_ ) );
        hopInCount_ = 0;
        outReadyCount_ = 0;
        outReadyPos_ = 0;
        baseSemitones_ = 0.0f;
        frameIdx_ = 0;
        blockSize_ = 8; swapsPct_ = 0.0f; swapSizePct_ = 0.0f; omitPct_ = 0.0f;
        mapSeed_ = 1u;
        identityMaps();
        randomBaseFn = defaultSeedDerivedBase;
        buildHann();
    }

    // Spectral-manipulation parameters (latched at render start via onNewRender).
    void setSpectral( int blockSize, float swapsPct, float swapSizePct, float omitPct )
    {
        blockSize_ = blockSize; swapsPct_ = swapsPct; swapSizePct_ = swapSizePct; omitPct_ = omitPct;
    }

    // Call once at the start of each new render (kit generation). Draws the base
    // pitch and (re)builds the swap/mute maps from the per-render seed.
    void onNewRender( uint32_t seed, int renderIndex )
    {
        float r = randomBaseFn ? randomBaseFn( seed, renderIndex ) : 0.5f;
        // Base pitch range: -12 .. +24 st (asymmetric, 2026-10-25 user request;
        // was +-24). Favours upward shifts while still allowing an octave down.
        baseSemitones_ = -12.0f + r * 36.0f;
        frameIdx_ = 0;
        // Decorrelate the map RNG from the base-pitch draw; ensure nonzero.
        mapSeed_ = ( seed * 2654435761u + (uint32_t)renderIndex * 40503u + 0x51ED2C19u ) | 1u;
        generateMaps();
    }

    // Block-latent by design (one block's worth, 1024 samples).
    float processSample( float x, float depthSemitones )
    {
        hopIn_[hopInCount_++] = x;
        if ( hopInCount_ == kHopSize )
        {
            hopInCount_ = 0;
            memmove( inBuf_, inBuf_ + kHopSize, ( kBlockSize - kHopSize ) * sizeof( float ) );
            memcpy( inBuf_ + kBlockSize - kHopSize, hopIn_, kHopSize * sizeof( float ) );

            float semis = baseSemitones_ + depthSemitones;
            if ( semis > 24.0f ) semis = 24.0f;
            if ( semis < -24.0f ) semis = -24.0f;
            processBlock( semis );

            memcpy( outReady_, outBuf_, kHopSize * sizeof( float ) );
            outReadyCount_ = kHopSize;
            outReadyPos_ = 0;
            memmove( outBuf_, outBuf_ + kHopSize, ( kBlockSize - kHopSize ) * sizeof( float ) );
            for ( int i = kBlockSize - kHopSize; i < kBlockSize; ++i ) outBuf_[i] = 0.0f;
        }
        if ( outReadyCount_ > 0 ) { --outReadyCount_; return outReady_[outReadyPos_++]; }
        return 0.0f;
    }

    float (*randomBaseFn)( uint32_t seed, int renderIndex );

private:
    // ---- small deterministic PRNG for map generation (no <random>) ----
    uint32_t rngState_;
    void rngSeed( uint32_t s ) { rngState_ = s ? s : 1u; }
    uint32_t rngNext() { uint32_t x = rngState_; x ^= x << 13; x ^= x >> 17; x ^= x << 5; rngState_ = x; return x; }
    int rngInt( int n ) { return n > 0 ? (int)( rngNext() % (uint32_t)n ) : 0; }

    void buildHann()
    {
        const float kTwoPi = 6.283185307f;
        for ( int i = 0; i < kBlockSize; ++i )
            hann_[i] = 0.5f - 0.5f * cosf( kTwoPi * i / kBlockSize );
    }

    void identityMaps()
    {
        for ( int k = 0; k < kNumBins; ++k ) { swapDst_[k] = (int16_t)k; muteGain_[k] = 1.0f; }
    }

    int numBlocks() const { return kOperable / blockSize_; }

    void generateMaps()
    {
        identityMaps();
        int B = blockSize_;
        int nB = numBlocks();
        if ( nB < 1 ) return;   // B>kOperable shouldn't happen (max B=64)

        int maxSwaps = nB / 2, maxSwapSz = nB / 2, maxOmit = nB;
        int swaps    = (int)( swapsPct_    * 0.01f * (float)maxSwaps  + 0.5f );
        int swapSize = (int)( swapSizePct_ * 0.01f * (float)maxSwapSz + 0.5f );
        int omit     = (int)( omitPct_     * 0.01f * (float)maxOmit   + 0.5f );
        if ( swapSize < 1 ) swapSize = 1;
        if ( swaps > maxSwaps ) swaps = maxSwaps;
        if ( omit  > maxOmit  ) omit  = maxOmit;

        rngSeed( mapSeed_ );

        // --- Mute map first (so a given seed reproduces both) ---
        for ( int i = 0; i < nB; ++i ) order_[i] = (int16_t)i;
        // partial Fisher-Yates to pick `omit` distinct blocks into order_[0..omit)
        for ( int i = 0; i < omit && i < nB; ++i )
        {
            int j = i + rngInt( nB - i );
            int16_t t = order_[i]; order_[i] = order_[j]; order_[j] = t;
        }
        for ( int i = 0; i < omit && i < nB; ++i )
        {
            int b = order_[i];
            for ( int j = 0; j < B; ++j ) muteGain_[1 + b * B + j] = 0.0f;
        }
        applyMuteRamps( B );

        // --- Swap map (block pairs, circular distance <= swapSize, wrap) ---
        for ( int i = 0; i < nB; ++i ) { blockDst_[i] = (int16_t)i; paired_[i] = 0; order_[i] = (int16_t)i; }
        // random visiting order
        for ( int i = 0; i < nB; ++i )
        {
            int j = i + rngInt( nB - i );
            int16_t t = order_[i]; order_[i] = order_[j]; order_[j] = t;
        }
        int pairsMade = 0;
        for ( int oi = 0; oi < nB && pairsMade < swaps; ++oi )
        {
            int a = order_[oi];
            if ( paired_[a] ) continue;
            for ( int attempt = 0; attempt < 16; ++attempt )
            {
                int d = 1 + rngInt( swapSize );               // 1..swapSize
                int sign = ( rngNext() & 1u ) ? 1 : -1;
                int b = ( ( a + sign * d ) % nB + nB ) % nB;   // wrap
                if ( b != a && !paired_[b] )
                {
                    blockDst_[a] = (int16_t)b; blockDst_[b] = (int16_t)a;
                    paired_[a] = paired_[b] = 1; ++pairsMade;
                    break;
                }
            }
        }
        // Expand block permutation to a per-bin table (leftover bins stay identity).
        for ( int b = 0; b < nB; ++b )
        {
            int disp = ( blockDst_[b] - b ) * B;
            for ( int j = 0; j < B; ++j )
            {
                int bin = 1 + b * B + j;
                swapDst_[bin] = (int16_t)( bin + disp );
            }
        }
    }

    // Raised-cosine ramp (R=min(B/4,4)) straddling each muted/unmuted boundary;
    // adjacent muted blocks merge into one hole (no internal boundary -> no ramp).
    // Two passes: pass 1 finds boundaries on the INTACT hard 0/1 mask (no writes),
    // pass 2 writes the ramps — so a written ramp value can't be mis-read as a
    // new boundary. For B>=4 (the only R>0 cases) muted runs are >=B bins apart
    // and 2R<B, so ramp windows never overlap and boundaries stay <= 254.
    void applyMuteRamps( int B )
    {
        int R = B / 4; if ( R > 4 ) R = 4;
        if ( R < 1 ) return;   // hard edges for B < 4
        const float kPi = 3.14159265f;

        int16_t bndK[256]; uint8_t bndMutedLeft[256]; int nBnd = 0;
        for ( int k = 1; k < kOperable && nBnd < 256; ++k )
        {
            bool mLo = ( muteGain_[k]   < 0.5f );
            bool mHi = ( muteGain_[k+1] < 0.5f );
            if ( mLo == mHi ) continue;
            bndK[nBnd] = (int16_t)k;
            bndMutedLeft[nBnd] = mLo ? 1 : 0;   // muted on the left side of the boundary?
            ++nBnd;
        }
        for ( int b = 0; b < nBnd; ++b )
        {
            int k = bndK[b];
            for ( int j = 0; j < 2 * R; ++j )
            {
                int bin = k - R + 1 + j;
                if ( bin < 1 || bin > kOperable ) continue;
                float t = ( (float)j + 0.5f ) / (float)( 2 * R );
                float val = 0.5f - 0.5f * cosf( kPi * t );   // 0 (left) -> 1 (right)
                if ( !bndMutedLeft[b] ) val = 1.0f - val;    // muted on the right: 1 -> 0
                muteGain_[bin] = val;
            }
        }
    }

    void processBlock( float shiftSemitones )
    {
        float ratio = powf( 2.0f, shiftSemitones / 12.0f );

        for ( int i = 0; i < kBlockSize; ++i ) { spec_[i].re = inBuf_[i] * hann_[i]; spec_[i].im = 0.0f; }
        fft::transform( spec_, kBlockSize, false );

        // Omit: copy the (muted) analysis of bins 0..kNumBins-1 into work_.
        for ( int k = 0; k < kNumBins; ++k )
        {
            work_[k].re = spec_[k].re * muteGain_[k];
            work_[k].im = spec_[k].im * muteGain_[k];
        }

        // Clear the output spectrum, then Swap-then-Shift each operable bin.
        for ( int i = 0; i < kBlockSize; ++i ) { spec_[i].re = 0.0f; spec_[i].im = 0.0f; }
        for ( int k = 1; k <= kOperable; ++k )
        {
            int postSwap = swapDst_[k];                       // swap (before shift)
            int dst = (int)( (float)postSwap * ratio + 0.5f ); // shift (bin relocate)
            if ( dst < 1 || dst > kOperable ) continue;        // drop out-of-range (as a bin-shift must)
            int delta = dst - k;                               // net displacement
            int idx = ( delta * frameIdx_ ) & ( kRotPeriod - 1 ); // j^(Δm), 0..3
            float re = work_[k].re, im = work_[k].im;
            float rre, rim;
            switch ( idx ) {                                   // multiply by {1, j, -1, -j}
                case 0:  rre =  re; rim =  im; break;
                case 1:  rre = -im; rim =  re; break;          // * j
                case 2:  rre = -re; rim = -im; break;          // * -1
                default: rre =  im; rim = -re; break;          // * -j
            }
            spec_[dst].re += rre; spec_[dst].im += rim;
        }

        // Rebuild conjugate-symmetric upper half for a real IFFT; DC/Nyquist = 0.
        for ( int dst = 1; dst <= kOperable; ++dst )
        {
            spec_[kBlockSize - dst].re =  spec_[dst].re;
            spec_[kBlockSize - dst].im = -spec_[dst].im;
        }
        spec_[0].re = spec_[0].im = 0.0f;
        spec_[kBlockSize / 2].re = spec_[kBlockSize / 2].im = 0.0f;

        fft::transform( spec_, kBlockSize, true );

        const float kOverlapAddScale = 0.5f;   // periodic Hann, 4x overlap COLA
        for ( int i = 0; i < kBlockSize; ++i )
            outBuf_[i] += spec_[i].re * kOverlapAddScale;

        ++frameIdx_;
    }

    float inBuf_[kBlockSize];
    float outBuf_[kBlockSize];
    float hopIn_[kHopSize];
    float outReady_[kHopSize];
    int   hopInCount_ = 0;
    int   outReadyCount_ = 0;
    int   outReadyPos_ = 0;

    float hann_[kBlockSize];
    float baseSemitones_ = 0.0f;
    int   frameIdx_ = 0;

    fft::Complex spec_[kBlockSize];   // working spectrum
    fft::Complex work_[kNumBins];     // post-omit analysis (source of the scatter)

    // Spectral-manipulation maps + parameters (per render).
    int     blockSize_ = 8;
    float   swapsPct_ = 0.0f, swapSizePct_ = 0.0f, omitPct_ = 0.0f;
    uint32_t mapSeed_ = 1u;
    float   muteGain_[kNumBins];      // per-bin mute gain (with raised-cosine ramps)
    int16_t swapDst_[kNumBins];       // per-bin swap destination (identity default)
    int16_t blockDst_[kOperable];     // block permutation scratch (map gen)
    int16_t order_[kOperable];        // shuffle scratch (map gen)
    uint8_t paired_[kOperable];       // paired flags (map gen)
};

} // namespace p1pitch
