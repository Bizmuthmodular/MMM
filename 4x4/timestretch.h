// timestretch.h — period-synchronous, read-time time stretch for the 4x4
// plugin's playback (per period-stretch.md, 2026-10-22). PLAYBACK ONLY: the
// kit audio in the play buffer is never modified; stretching is purely how the
// read head moves through it. Each buffer owns a set of period maps built once
// per render (post-render analysis phase in renderer.h) and swapped together
// with the audio.
//
// Concept: look for periodic repetitions of a chosen length and extend them by
// replaying whole periods with a crossfade at each splice. Non-periodic
// material (confidence below Threshold) plays at 1x. One "Scale" knob selects
// the target repetition length across an octave-spaced set; "Stretch" sets how
// much (1x..8x = finite plays per slice); "Threshold" the minimum periodicity; "Xfade"
// the splice crossfade length.
//
// Design decisions (confirmed 2026-10-22): Pitch (playback speed) and Stretch
// coexist — the existing pitch ratio is the per-sample advance `adv`, and
// period-repeats extend duration on top (pitch preserved at short scales). The
// player keeps LOOPING the kit; a finished render swaps in immediately (after
// its analysis completes). At Stretch = 1x this engine is bit-identical to the
// old plain looping player.
//
// Simplifications vs the spec (acceptable for a creative effect, flagged for
// on-device tuning): analysis decimates by subsample/box-average (not a full
// anti-alias LP); no full-rate period refine and no splice-time WSOLA refine in
// v1 (the linear crossfade covers the <=D-sample period error). These can be
// added later if short-scale splices sound clicky on hardware.
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11, <cmath>/<cstdint>.
#pragma once
#include "kit_buffer.h"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace timestretch {

// Octave-spaced target lengths (ms). Top capped at 1024 ms so the longest
// full-rate period/lag fits a uint16 at the kit's 32 kHz rate (1024ms*sqrt2 =
// ~46k samples < 65535) and so a ~4.4s kit still repeats such a period a few
// times. Compile-time constants.
static constexpr int kNumScales = 10;
static constexpr int kScaleMs[kNumScales] = { 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024 };
static constexpr int kRate = kitbuffer::kSampleRateHz;        // 32000
static constexpr int kMinHop = 256;                           // samples
static constexpr int kScratchMax = 320;                       // decimated window cap (M ~ 181)

// Per-scale geometry — all constexpr so kTotalEntries is known at compile time.
constexpr int cLen( int s )   { return kScaleMs[s] * kRate / 1000; }               // scale length, samples
constexpr int cHop( int s )   { return ( cLen(s) / 2 ) < kMinHop ? kMinHop : ( cLen(s) / 2 ); }
constexpr int cCount( int s ) { return kitbuffer::kNumSamples / cHop(s) + 2; }     // map entries for scale s
constexpr int cSum( int s )   { return s < 0 ? 0 : cCount(s) + cSum(s - 1); }
static constexpr int kTotalEntries = cSum( kNumScales - 1 );

// Flat offset of scale s in the per-buffer map arrays (cheap, s < 10).
inline int scaleOffset( int s ) { int o = 0; for ( int i = 0; i < s; ++i ) o += cCount(i); return o; }

// One set of period maps per buffer (packed: parallel arrays, 3 bytes/entry).
struct PeriodMaps
{
    uint16_t period[kTotalEntries];   // detected period, full-rate samples
    uint8_t  conf[kTotalEntries];     // normalized autocorr peak, 0..255

    void clear() { memset( period, 0, sizeof( period ) ); memset( conf, 0, sizeof( conf ) ); }
};

// Read-time parameters (all safe to change continuously).
struct Params
{
    float scale01;      // 0..1 across the scale set
    float stretch;      // 1..8 = plays per slice (finite; 1 = passthrough)
    float threshold01;  // 0..1 minimum periodicity confidence
    float xfadeMs;      // splice crossfade, capped at period/2
};

// ---------------------------------------------------------------------------
// Analyzer — builds a PeriodMaps from a (finished) kit buffer, chunked across
// step() calls so the per-call cost stays bounded (runs in the renderer's
// post-render kAnalyzing phase, never in the real-time feedback render).
// ---------------------------------------------------------------------------
class Analyzer
{
public:
    void begin( const kitbuffer::KitBuffer* kit, PeriodMaps* maps )
    {
        kit_ = kit; maps_ = maps; scale_ = 0; hopIdx_ = 0; done_ = false;
    }

    // Process up to `budgetHops` hops. Returns true once every scale/hop done.
    bool advance( int budgetHops )
    {
        for ( int b = 0; b < budgetHops && !done_; ++b )
        {
            analyzeOneHop( scale_, hopIdx_ );
            if ( ++hopIdx_ >= cCount( scale_ ) )
            {
                hopIdx_ = 0;
                if ( ++scale_ >= kNumScales ) done_ = true;
            }
        }
        return done_;
    }

    bool done() const { return done_; }

private:
    void analyzeOneHop( int s, int i )
    {
        const int N = kitbuffer::kNumSamples;
        const int hop = cHop( s );
        const int L = cLen( s );
        const int p = i * hop;
        int lagLo = (int)( L * 0.70710678f );   // L / sqrt(2)
        int lagHi = (int)( L * 1.41421356f );    // L * sqrt(2)
        int D = L / 64; if ( D < 1 ) D = 1;
        const bool envelope = ( L >= 3200 );     // >=100ms: periodicity lives in the envelope

        int M = ( 2 * lagHi ) / D;
        if ( M > kScratchMax ) M = kScratchMax;
        if ( M < 4 ) M = 4;

        // Build the decimated analysis window (wrapped reads — playback loops).
        float mean = 0.0f;
        for ( int j = 0; j < M; ++j )
        {
            int base = p + j * D;
            float v;
            if ( envelope )
            {
                int idx = base % N; if ( idx < 0 ) idx += N;
                v = kit_->read( idx ); if ( v < 0.0f ) v = -v;   // rectified single sample
            }
            else
            {
                float acc = 0.0f;
                for ( int k = 0; k < D; ++k ) { int idx = ( base + k ) % N; acc += kit_->read( idx ); }
                v = acc / (float)D;                              // box-average
            }
            scratch_[j] = v;
            mean += v;
        }
        mean /= (float)M;
        for ( int j = 0; j < M; ++j ) scratch_[j] -= mean;       // zero-mean (kills DC, esp. for envelope)

        // Normalized autocorrelation over decimated lags.
        int lagLoD = lagLo / D; if ( lagLoD < 1 ) lagLoD = 1;
        int lagHiD = lagHi / D; if ( lagHiD >= M ) lagHiD = M - 1;
        int Wd = M - lagHiD; if ( Wd < 1 ) Wd = 1;

        float e0 = 0.0f;
        for ( int n = 0; n < Wd; ++n ) e0 += scratch_[n] * scratch_[n];

        float bestR = 0.0f; int bestLagD = lagLoD;
        for ( int tau = lagLoD; tau <= lagHiD; ++tau )
        {
            float num = 0.0f, e1 = 0.0f;
            for ( int n = 0; n < Wd; ++n )
            {
                float a = scratch_[n], b = scratch_[n + tau];
                num += a * b; e1 += b * b;
            }
            float denom = e0 * e1;
            float r = denom > 1e-12f ? num / sqrtf( denom ) : 0.0f;
            if ( r > bestR ) { bestR = r; bestLagD = tau; }
        }

        int periodFull = bestLagD * D;
        if ( periodFull < 1 ) periodFull = 1;
        if ( periodFull > 65535 ) periodFull = 65535;
        float conf = bestR; if ( conf < 0.0f ) conf = 0.0f; if ( conf > 1.0f ) conf = 1.0f;

        int off = scaleOffset( s );
        maps_->period[off + i] = (uint16_t)periodFull;
        maps_->conf[off + i] = (uint8_t)( conf * 255.0f + 0.5f );
    }

    const kitbuffer::KitBuffer* kit_ = nullptr;
    PeriodMaps* maps_ = nullptr;
    int  scale_ = 0, hopIdx_ = 0;
    bool done_ = true;
    float scratch_[kScratchMax];
};

// ---------------------------------------------------------------------------
// StretchReader — the read-head / splice / crossfade engine. One output sample
// per process() call. At Stretch = 1x it is bit-identical to a plain
// interpolated looping read (see passthrough note in process()).
// ---------------------------------------------------------------------------
class StretchReader
{
public:
    StretchReader() { reset(); }

    void reset()
    {
        pos_ = 0.0; segStart_ = 0.0; period_ = 512.0; debt_ = 0.0;
        fadePosOld_ = 0.0; fadeG_ = 0.0f; fadeStep_ = 0.0f; curConf_ = 0.0f;
        repeatsThisSeg_ = 0; primed_ = false; fading_ = false;
    }
    void onSwap() { reset(); }
    // Restart the read head at a given buffer position (used by a note-gated
    // player on note-on, so playback begins at its StartPos).
    void start( double startPos ) { reset(); pos_ = startPos; }

    // kit + its maps (must belong together), params, per-sample advance `adv`
    // (= kit/out rate * pitch ratio), and the playback normalization gain.
    float process( const kitbuffer::KitBuffer& kit, const PeriodMaps& maps,
                   const Params& prm, double adv, float normGain )
    {
        const int N = kitbuffer::kNumSamples;

        if ( !primed_ )
        {
            debt_ = 0.0; fading_ = false;
            enterSegment( maps, prm, pos_ );
            primed_ = true;
        }

        float main = interp( kit, pos_ );
        float out;
        if ( fading_ )
        {
            float old = interp( kit, fadePosOld_ );
            out = ( 1.0f - fadeG_ ) * old + fadeG_ * main;   // incoming (main) fades in, outgoing fades out
            fadeG_ += fadeStep_;
            fadePosOld_ += adv;
            if ( fadeG_ >= 1.0f ) fading_ = false;
        }
        else
        {
            out = main;
        }

        pos_ += adv;

        // Loop wrap: reset the segment at the wrapped position.
        bool wrapped = false;
        while ( pos_ >= (double)N ) { pos_ -= (double)N; wrapped = true; }
        if ( wrapped )
        {
            debt_ = 0.0; fading_ = false;      // fresh start each loop pass
            enterSegment( maps, prm, pos_ );
        }

        // Period boundary reached?
        if ( period_ >= 1.0 && pos_ >= segStart_ + period_ )
        {
            // FINITE repeats: the "owed" repeats for this slice were accrued
            // ONCE on entering the segment (enterSegment: debt += sEff-1). Here
            // we just pay them down — repeat while we still owe a whole period
            // (and under the safety cap), otherwise ADVANCE to the next slice.
            // Because the owed amount is bounded (sEff-1 <= 7) the segment always
            // advances after a finite number of repeats — it can never get stuck
            // on one slice. (Old bug: debt was incremented every boundary, so at
            // Stretch>=2x with high confidence it repeated forever.)
            if ( debt_ >= 1.0 && repeatsThisSeg_ < kMaxRepeats && ( pos_ - period_ ) >= 0.0 )
            {
                // Replay the current segment: outgoing head continues forward,
                // incoming head jumps back one period; crossfade between them.
                double fadeLen = period_ * 0.5;
                double xf = (double)prm.xfadeMs * 0.001 * (double)kitbuffer::kSampleRateHz;
                if ( xf < fadeLen ) fadeLen = xf;
                if ( fadeLen < 1.0 ) fadeLen = 1.0;
                fadePosOld_ = pos_;
                pos_ -= period_;
                fading_ = true; fadeG_ = 0.0f; fadeStep_ = (float)( 1.0 / fadeLen );
                debt_ -= 1.0;
                ++repeatsThisSeg_;
                // segStart_ / period_ unchanged — same segment repeats.
            }
            else
            {
                enterSegment( maps, prm, pos_ );   // advance to the next slice
            }
        }

        return out * normGain;
    }

private:
    // Enter a new slice starting at `pos`: look up its period/confidence, and
    // accrue the finite repeats it is owed — ONCE, here, not per boundary.
    // sEff = 1 + (stretch-1)*w is the confidence-weighted play count (w=0 below
    // Threshold => no repeats => plays through at 1x). debt carries the
    // fractional remainder between slices so non-integer Stretch averages out.
    void enterSegment( const PeriodMaps& maps, const Params& prm, double pos )
    {
        segStart_ = pos;
        double per; float conf;
        chooseScale( maps, prm, pos, per, conf );
        period_ = per; curConf_ = conf;
        float w = weight( conf, prm.threshold01 );
        float sEff = 1.0f + ( prm.stretch - 1.0f ) * w;
        debt_ += (double)( sEff - 1.0f );
        repeatsThisSeg_ = 0;
    }

    float interp( const kitbuffer::KitBuffer& kit, double pos ) const
    {
        const int N = kitbuffer::kNumSamples;
        int i0 = (int)pos; if ( i0 < 0 ) i0 = 0; if ( i0 >= N ) i0 = N - 1;
        int i1 = i0 + 1; if ( i1 >= N ) i1 = 0;
        float frac = (float)( pos - (double)i0 );
        return kit.read( i0 ) * ( 1.0f - frac ) + kit.read( i1 ) * frac;
    }

    // Pick the target period/confidence at `pos` from the two scales the knob
    // straddles, by confidence-weighted score (never averages the periods — an
    // averaged period matches no real periodicity and won't splice cleanly).
    void chooseScale( const PeriodMaps& maps, const Params& prm, double pos,
                      double& outPeriod, float& outConf ) const
    {
        float sf = prm.scale01 * (float)( kNumScales - 1 );
        if ( sf < 0.0f ) sf = 0.0f;
        int sLo = (int)sf;
        if ( sLo > kNumScales - 1 ) sLo = kNumScales - 1;
        int sHi = sLo + 1;
        if ( sHi > kNumScales - 1 ) sHi = kNumScales - 1;
        float frac = sf - (float)sLo;

        uint16_t pLo, pHi; uint8_t cLo, cHi;
        lookupScale( maps, sLo, pos, pLo, cLo );
        lookupScale( maps, sHi, pos, pHi, cHi );

        if ( sHi == sLo )            { outPeriod = (double)pLo; outConf = cLo / 255.0f; }
        else
        {
            float scoreLo = ( cLo / 255.0f ) * ( 1.0f - frac );
            float scoreHi = ( cHi / 255.0f ) * frac;
            if ( scoreHi > scoreLo ) { outPeriod = (double)pHi; outConf = cHi / 255.0f; }
            else                     { outPeriod = (double)pLo; outConf = cLo / 255.0f; }
        }
        if ( outPeriod < 1.0 ) outPeriod = 1.0;
    }

    void lookupScale( const PeriodMaps& maps, int s, double pos, uint16_t& per, uint8_t& conf ) const
    {
        int hop = cHop( s );
        int idx = (int)( pos / (double)hop );
        int cnt = cCount( s );
        if ( idx < 0 ) idx = 0;
        if ( idx >= cnt ) idx = cnt - 1;
        int off = scaleOffset( s );
        per = maps.period[off + idx];
        conf = maps.conf[off + idx];
    }

    static float weight( float conf, float threshold )
    {
        if ( conf <= threshold ) return 0.0f;
        float w = ( conf - threshold ) / ( 1.0f - threshold + 1e-6f );
        return w > 1.0f ? 1.0f : w;   // soft ramp from Threshold up to full
    }

    // Safety cap on repeats per slice. The debt model already self-limits to
    // ~(stretch-1) <= 7 repeats, so this never triggers in normal use — it just
    // guarantees "never stuck" even if confidence/debt did something pathological.
    static const int kMaxRepeats = 16;

    double pos_, segStart_, period_, debt_, fadePosOld_;
    float  fadeG_, fadeStep_, curConf_;
    int    repeatsThisSeg_;
    bool   primed_, fading_;
};

} // namespace timestretch
