// contour_destinations.h — Contour modulation, single-contour "playhead"
// design (2026-10-21, user request: "a single contour ... with as many
// playheads as modulation destinations ... readout position offsets on the
// contour slope calculated in degrees; all heads complete a full pass (360)
// but with randomised offsets so everything doesn't move together").
//
// WHAT CHANGED (and why): the earlier design ran ONE lazy contourlazy::Engine
// PER destination (21 of them), each advanced every internal render sample.
// That was the dominant per-sample CPU cost during a bake and, because the
// bake runs inside the real-time step() callback, its periodic cost spikes
// (alongside P1's FFT) pushed the module past its 90% CPU mute threshold —
// the "render bleed" the user heard as periodic cracking in playback. It was
// also 21*1504 B of Contour state.
//
// NOW: ONE contour curve is generated per render and baked into a small
// cyclic lookup table (kTableSize points over the whole render = 360deg).
// Each of the kNumDestinations destinations is a "playhead": a readout
// position = a fixed, seed-derived phase OFFSET (in [0,1) = 0..360deg) added
// to a single base phase that sweeps 0->1 across the render. So every head
// traverses the entire slope once (a "full pass"), but each starts at a
// different point, so the destinations are decorrelated instead of moving in
// lockstep. Per sample this is now 1 cheap table LERP per destination (no
// per-destination envelope generation at all) — a large CPU and memory cut.
//
// Per-render randomisation (2026-10-21, user request: "the contour should
// also be randomised based on its parameters and a new seed each render"):
// onNewRender() takes a fresh per-render seed (the renderer passes
// baseSeed + renderIndex, the SAME seed the processor bases use), and BOTH the
// baked curve shape AND the playhead phase offsets are re-derived from it. So
// every render gets a new modulation gesture (still governed by the settings
// bank — Windows/Chaos/Radius/Round/Hold), not just new processor bases. It
// stays deterministic: the same seed (i.e. same baseSeed + render index)
// reproduces the same modulation. Mapping ranges (crosspoint +-1, P1 +-12 st,
// P2 +-500 Hz, P3 +-50 ms, P4 phaser +-2000 Hz, filter-Q 1..96 st) are
// unchanged from the per-engine design. (This costs nothing extra: the table
// was already re-baked every render; only the seed now varies.)
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11.
#pragma once
#include "contour_lazy.h"
#include "feedback_network.h"
#include <cstdint>
#include <cmath>

namespace contourdest {

static const int kNumCrosspoints  = 16;
static const int kNumDestinations = 21;   // 16 crosspoints + P1/P2/P3 depth + P4 phaser + filter-Q
static const int kTableSize       = 1024; // cyclic curve table (points over the whole render)

// Destination indices into the playhead/value arrays (stable order).
enum {
    kDestCrosspoint0 = 0,                 // 0..15
    kDestP1Depth = kNumCrosspoints,       // 16
    kDestP2Depth,                         // 17
    kDestP3Delay,                         // 18
    kDestP4Freq,                          // 19
    kDestQMod,                            // 20
};

// Shared settings bank (§6): shape the single contour curve.
struct SharedSettings
{
    int   windows   = 6;
    float chaos     = 0.6f;
    float radiusSec = 0.12f;
    float roundProb = 1.0f;
    float holdProb  = 0.0f;
};

class ContourDestinations
{
public:
    // Set the single source engine's SHAPE params from the settings bank and
    // the base-phase increment for this sample rate. Called at render start
    // only when the settings or the (oversampled) rate change (renderer owns
    // that decision). The seed + phase offsets set here are just an initial
    // default — onNewRender() re-seeds BOTH the curve and the offsets from the
    // fresh per-render seed every render.
    void configure( const SharedSettings& shared, float lengthSec, uint32_t baseSeed, float sampleRate )
    {
        contourlazy::Params p;
        p.windows   = shared.windows;
        p.chaos     = shared.chaos;
        p.radiusSec = shared.radiusSec;
        p.roundProb = shared.roundProb;
        p.holdProb  = shared.holdProb;
        p.levelV    = 5.0f;
        p.offsetV   = 0.0f;
        p.bipolar   = true;     // +-5V curve, same convention the mappings expect
        p.loop      = false;
        p.lengthSec = lengthSec;
        p.seed      = baseSeed;
        engine_.params = p;
        engine_.rebuild();
        engine_.setSampleRate( sampleRate );

        for ( int i = 0; i < kNumDestinations; ++i )
            offset_[i] = offsetForDestination( baseSeed, i );   // phase offset in [0,1) == degrees/360

        lengthSec_ = lengthSec;
        // Base phase sweeps exactly 0->1 across the render's internal samples,
        // so each playhead completes one full 360deg pass over the kit duration
        // regardless of oversampling.
        double total = (double)lengthSec * (double)sampleRate;
        phaseInc_ = total > 1.0 ? 1.0 / total : 0.0;
    }

    // Start a new render: reseed the contour with this render's seed so the
    // curve shape AND the playhead offsets are freshly randomised this render
    // (shape still governed by the settings bank), then bake the cyclic table
    // and reset the base phase. Cheap, one-time (one curve rebuild + kTableSize
    // evaluations) — no per-sample engine work for the rest of the render.
    // Deterministic: the same renderSeed reproduces the same modulation.
    void onNewRender( uint32_t renderSeed )
    {
        engine_.params.seed = renderSeed;             // fresh gesture each render
        engine_.renderCurve( table_, kTableSize );    // temp engine rebuilds from params (new seed)
        for ( int i = 0; i < kNumDestinations; ++i )
            offset_[i] = offsetForDestination( renderSeed, i );   // re-stagger the playheads too
        phase_ = 0.0;
        // Prime the cache so accessors are valid before the first advance().
        sampleAllHeads();
    }

    // Advance the base phase one internal render sample and cache every
    // playhead's value. (Compute-then-advance, matching the old process()
    // "return current, then step" ordering.)
    void advance()
    {
        sampleAllHeads();
        phase_ += phaseInc_;
        if ( phase_ >= 1.0 ) phase_ -= 1.0;
    }

    // --- Output-unit mapping layer (unchanged ranges) ---

    float crosspointGain( int row, int col ) const { return val_[row * 4 + col] / 5.0f; }

    float p1DepthSemitones() const { return val_[kDestP1Depth] * ( 12.0f / 5.0f ); }
    float p2DepthHz()        const { return val_[kDestP2Depth] * ( 500.0f / 5.0f ); }
    float p3DelayMsOffset()  const { return val_[kDestP3Delay] * ( 50.0f / 5.0f ); }
    float p4FreqOffset()     const { return val_[kDestP4Freq]  * ( 2000.0f / 5.0f ); }   // P4 phaser: +-2000 Hz

    // Shared filter-Q (bandpass bandwidth) modulation, used only when QCONT is
    // on. Maps the bipolar +-5V curve linearly to the full 1..96 semitone range.
    float qBandwidthSemis() const
    {
        float bw = 1.0f + ( val_[kDestQMod] + 5.0f ) / 10.0f * 95.0f;
        if ( bw < 1.0f ) bw = 1.0f;
        if ( bw > 96.0f ) bw = 96.0f;
        return bw;
    }

    void fillChannelControls( feedbacknet::ChannelControls& ctrl ) const
    {
        ctrl.p1DepthSemitones = p1DepthSemitones();
        ctrl.p2DepthHz        = p2DepthHz();
        ctrl.p3DelayMs        = p3DelayMsOffset();
        ctrl.p4FreqOffset     = p4FreqOffset();
    }

    void fillMatrix( feedbacknet::FeedbackNetwork& net ) const
    {
        for ( int row = 0; row < 4; ++row )
            for ( int col = 0; col < 4; ++col )
                net.setMatrixCoeff( row, col, crosspointGain( row, col ) );
    }

private:
    // Seed-derived phase offset in [0,1) (== degrees/360) for destination i.
    // mulberry32-style finalizer; distinct from the processors' base hashes.
    static float offsetForDestination( uint32_t seed, int i )
    {
        uint32_t x = seed * 2246822519u + (uint32_t)i * 3266489917u + 0x9E3779B9u;
        x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
        return (float)x / 4294967296.0f;
    }

    // Cyclic linear interpolation of the baked curve at normalized phase [0,1).
    float sampleTable( double phase01 ) const
    {
        double fidx = phase01 * (double)kTableSize;
        int i0 = (int)fidx;
        if ( i0 >= kTableSize ) i0 -= kTableSize;
        if ( i0 < 0 ) i0 = 0;
        int i1 = i0 + 1; if ( i1 >= kTableSize ) i1 = 0;   // wrap -> cyclic
        float frac = (float)( fidx - (double)i0 );
        return table_[i0] * ( 1.0f - frac ) + table_[i1] * frac;
    }

    void sampleAllHeads()
    {
        for ( int i = 0; i < kNumDestinations; ++i )
        {
            double ph = phase_ + (double)offset_[i];
            ph -= floor( ph );   // frac to [0,1)
            val_[i] = sampleTable( ph );
        }
    }

    contourlazy::Engine engine_;               // single source curve
    float   table_[kTableSize] = {};           // baked cyclic curve (volts, +-5)
    float   offset_[kNumDestinations] = {};    // playhead phase offsets, [0,1) == degrees/360
    float   val_[kNumDestinations] = {};       // cached playhead values this sample
    double  phase_ = 0.0;                      // base phase, sweeps 0->1 over the render
    double  phaseInc_ = 0.0;
    float   lengthSec_ = 6.0f;
};

} // namespace contourdest
