// feedback_network.h — 4x4 feedback matrix, fixed injection points,
// starter-wave generator, and the per-channel Bandpass/Reg chain.
// Per 4x4-System-Overview.md §1 (per-channel filter is a resonant bandpass
// as of 2026-10-20, replacing the former HPF+LPF):
//
//   Sn ──────────────────────────────────────────┐
//    (fixed, direct)                              ├──► Σn ──► Bandpass ──► Pn ──► Reg ──┬──► Out n
//   P1..P4 ─► [Feedback mixer 4x4, bipolar] ───────┘                                    │
//      ▲                                                                                │
//      └────────────────────────────────────────────────────────────────────────────────┘
//
// IMPLEMENTATION NOTE on feedback timing: a digital feedback loop cannot
// resolve a same-sample circular dependency (channel n's output would
// depend on channel m's output which depends on channel n's output,
// ad infinitum, within the same sample) without iterative solving, which
// the doc doesn't call for. The standard, necessary structural choice —
// used here — is a one-sample delay in the loop: each channel's feedback
// mix at sample t uses every channel's Reg OUTPUT from sample t-1, not t.
// This is implicit in any real analog patch too (signals take nonzero time
// to propagate through a cable), so it isn't a deviation from the design,
// just the concrete form a feedback loop has to take in a sample-by-sample
// digital implementation.
//
// Each of the 4 channels is hard-wired to one specific processor type —
// channel 1 = P1 (pitch shift), channel 2 = P2 (frequency shift),
// channel 3 = P3 (delay — the wavefolder was omitted 2026-10-19),
// channel 4 = P4 (12-stage allpass phaser — the bitcrusher was omitted
// 2026-10-21, user request) — matching
// §2-§5's naming (P1..P4 are specific processor types, not 4 interchangeable
// instances of one generic processor). Each processor's own Contour-driven
// control offsets (depth/delay/bits/divisor) are carried on ChannelControls;
// each channel's resonant bandpass (static Freq + Bandwidth) is set via
// setBandpassParams() at render start, not per sample.
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11.
//
// REQUIRES fft::initTwiddles() TO HAVE BEEN CALLED ONCE before this type is
// used at all (channel 1 wraps a p1pitch::Processor internally) -- see
// p1_pitchshift.h. Omitting it fails silently: every channel-1 output is
// exactly 0.0, with no error. In the real plugin this belongs in the
// factory's initialise() callback (Step 15); every standalone test file
// calls it explicitly at the top of main() instead.
#pragma once
#include "p1_pitchshift.h"
#include "p2_freqshift.h"
#include "delay.h"            // P3 is now a plain delay (wavefolder omitted, 2026-10-19)
#include "p4_phaser.h"       // P4 is now a 12-stage allpass phaser (bitcrusher omitted, 2026-10-21)
#include "filters.h"
#include <cstdint>
#include <cstring>

namespace feedbacknet {

// Starter-wave generator (§1 "Starter wave injection"): one phase accumulator
// per channel, fires once for durationSec at the start of each render then goes
// silent. Waveform (square/saw/triangle/noise), frequency and duration are
// randomized per channel per render by the renderer (2026-10-24 user request).
class StarterWave
{
public:
    enum { kSquare = 0, kSaw = 1, kTriangle = 2, kNoise = 3 };

    StarterWave() : freqHz( 110.0f ), durationSec( 0.2f ), waveform( kSquare ),
                    phase_( 0.0f ), sampleIndex_( 0 ), rng_( 0x12345u ) {}

    float    freqHz;       // randomized per render within [MinFreq, MaxFreq]
    float    durationSec;  // randomized per render within [MinLength, MaxLength]
    int      waveform;     // randomized per render: square/saw/triangle/noise

    void onNewRender( uint32_t seed )
    {
        phase_ = 0.0f;
        sampleIndex_ = 0;
        rng_ = seed | 1u;   // deterministic noise per render
    }

    float processSample( float sampleRate )
    {
        float durationSamples = durationSec * sampleRate;
        float out = 0.0f;
        if ( (float)sampleIndex_ < durationSamples )
        {
            switch ( waveform )
            {
            case kSaw:      out = ( phase_ * 2.0f - 1.0f ) * 5.0f; break;                 // rising ramp -5..+5
            case kTriangle: out = ( phase_ < 0.5f ? ( phase_ * 4.0f - 1.0f )
                                                  : ( 3.0f - phase_ * 4.0f ) ) * 5.0f; break;
            case kNoise:    out = ( (float)nextRng() / 2147483648.0f - 1.0f ) * 5.0f; break; // white +-5V
            default:        out = ( phase_ < 0.5f ) ? 5.0f : -5.0f; break;                // square
            }
            phase_ += freqHz / sampleRate;
            if ( phase_ >= 1.0f ) phase_ -= 1.0f;
        }
        ++sampleIndex_;
        return out;
    }

private:
    uint32_t nextRng() { uint32_t x = rng_; x ^= x << 13; x ^= x >> 17; x ^= x << 5; rng_ = x; return x; }

    float    phase_;
    uint32_t sampleIndex_;
    uint32_t rng_;
};

// Per-sample, Contour-driven control offsets each processor needs. (The
// per-channel bandpass filter is configured separately via
// setBandpassParams() since its Freq/Bandwidth are static per render.)
struct ChannelControls
{
    // Per-channel bandpass (Freq + Bandwidth) is set via setBandpassParams()
    // at render start, NOT carried here per sample (the filter params are
    // static per render). So ChannelControls only carries the per-sample
    // Contour-driven offsets and the enable flags.
    float p1DepthSemitones;    // Contour-driven, depth-scaled OFFSETs added to each
    float p2DepthHz;           // processor's own randomized per-render base (§2/§3)
    float p3DelayMs;           // P3 is now a delay (wavefolder omitted); offset in ms
    float p4FreqOffset;        // P4 is now a phaser; signed Hz offset on its swept center freq
    // Per-processor on/off (channel n = Pn). When false the channel's input
    // AND output are muted: it receives no signal and contributes nothing to
    // either the final mix or the feedback matrix (its processor is also
    // skipped entirely, saving CPU). Default all enabled.
    bool  processorEnabled[4];

    ChannelControls() :
        p1DepthSemitones( 0.0f ), p2DepthHz( 0.0f ),
        p3DelayMs( 0.0f ), p4FreqOffset( 0.0f )
    {
        for ( int n = 0; n < 4; ++n ) processorEnabled[n] = true;
    }
};

class FeedbackNetwork
{
public:
    FeedbackNetwork() { reset(); }

    void reset()
    {
        memset( matrix_, 0, sizeof( matrix_ ) );
        memset( prevOut_, 0, sizeof( prevOut_ ) );
        p1_.reset();
        p2_.reset();
        delay_.reset();
        p4_.reset();
        for ( int n = 0; n < 4; ++n )
        {
            starter_[n].onNewRender( 0x100u + (uint32_t)n );
            bp_[n].reset();
            reg_[n].reset();
        }
    }

    // Set a channel's static bandpass (center freq Hz, bandwidth semitones) at
    // the current internal rate. Called by the renderer at each render start.
    void setBandpassParams( int ch, float freqHz, float bwSemitones, float sampleRate )
    {
        bp_[ch].setParams( freqHz, bwSemitones, sampleRate );
    }

    // P1 spectral-manipulation params (BlockSize + Swaps/SwapSize/Omit %),
    // latched into the maps at the next render start (p1_.onNewRender).
    void setP1Spectral( int blockSize, float swapsPct, float swapSizePct, float omitPct )
    {
        p1_.setSpectral( blockSize, swapsPct, swapSizePct, omitPct );
    }

    // Bipolar attenuverter coefficient, row = destination channel (Sigma n),
    // col = source channel (Out m). Caller is responsible for keeping
    // values within [-1,1] per §1's "bipolar attenuverters, -1...+1".
    void setMatrixCoeff( int row, int col, float v ) { matrix_[row][col] = v; }
    float matrixCoeff( int row, int col ) const { return matrix_[row][col]; }

    StarterWave& starter( int channel ) { return starter_[channel]; }

    // Call once at the start of each new render (kit generation).
    void onNewRender( uint32_t seed )
    {
        for ( int n = 0; n < 4; ++n )
        {
            starter_[n].onNewRender( seed + (uint32_t)n * 2654435761u );
            prevOut_[n] = 0.0f;
            bp_[n].reset();   // state only; coeffs re-set by the renderer post-rate-change
            reg_[n].reset();
        }
        p1_.reset();
        p2_.reset();
        delay_.reset();
        p4_.reset();
        p1_.onNewRender( seed, 0 );
        p2_.onNewRender( seed, 1 );
        delay_.onNewRender( seed, 2 );
        p4_.onNewRender( seed, 3 );
    }

    // One render-sample across all 4 channels. outp[0..3] receives each
    // channel's fully-regulated Out n (post Reg), which is also what feeds
    // back into the matrix next sample.
    void processSample( float sampleRate, const ChannelControls& ctrl, float outp[4] )
    {
        float sigma[4];
        for ( int n = 0; n < 4; ++n )
        {
            // Advance the starter unconditionally so its one-shot timeline
            // stays aligned regardless of enable state; a disabled channel
            // simply discards it (input muted -> sigma = 0).
            float s = starter_[n].processSample( sampleRate );
            if ( !ctrl.processorEnabled[n] ) { sigma[n] = 0.0f; continue; }
            float fb = 0.0f;
            for ( int m = 0; m < 4; ++m ) fb += matrix_[n][m] * prevOut_[m];
            sigma[n] = s + fb;
        }

        float filtered[4];
        for ( int n = 0; n < 4; ++n )
        {
            if ( !ctrl.processorEnabled[n] ) { filtered[n] = 0.0f; continue; }
            filtered[n] = bp_[n].process( sigma[n] );   // static per-channel resonant bandpass
        }

        // Disabled channels skip their processor entirely (CPU saved — matters
        // most for P1's FFT). procOut for a disabled channel is unused.
        float procOut[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if ( ctrl.processorEnabled[0] ) procOut[0] = p1_.processSample( filtered[0], ctrl.p1DepthSemitones );
        if ( ctrl.processorEnabled[1] ) procOut[1] = p2_.processSample( filtered[1], ctrl.p2DepthHz, sampleRate );
        if ( ctrl.processorEnabled[2] ) procOut[2] = delay_.processSample( filtered[2], ctrl.p3DelayMs, sampleRate );
        if ( ctrl.processorEnabled[3] ) procOut[3] = p4_.processSample( filtered[3], ctrl.p4FreqOffset, sampleRate );

        for ( int n = 0; n < 4; ++n )
        {
            if ( !ctrl.processorEnabled[n] )
            {
                // Output muted: contributes nothing to the mix, and feeds 0
                // into feedback-matrix column n next sample.
                prevOut_[n] = 0.0f;
                outp[n] = 0.0f;
                continue;
            }
            float y = reg_[n].process( procOut[n], sampleRate );
            // Denormal flush on the recirculating loop state. In a decaying
            // resonant feedback loop the fed-back value can drift into
            // denormal range; on the Cortex-M7 FPU (unlike desktop x86) that
            // can be far slower to handle, risking a CPU spike that mutes the
            // module. Flushing sub-audible tiny values to 0 is free insurance
            // and audibly a no-op. (2026-10-20, alongside the DRAM fix.)
            if ( fabsf( y ) < 1e-20f ) y = 0.0f;
            prevOut_[n] = y;
            outp[n] = y;
        }
    }

private:
    float matrix_[4][4];
    float prevOut_[4];
    StarterWave starter_[4];
    filters::Bandpass bp_[4];    // per-channel resonant bandpass before each Pn
    filters::Reg reg_[4];
    p1pitch::Processor p1_;
    p2freqshift::Processor p2_;
    p3delay::Processor delay_;   // channel 2 (P3), replaces the former wavefolder
    p4phaser::Processor p4_;     // channel 3 (P4): 12-stage allpass phaser
};

} // namespace feedbacknet
