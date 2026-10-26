// 4x4.cpp — 4x4 Matrix Feedback Generator, full plugin integration.
// Implementation Plan Step 15. First actually-loadable, actually-playable
// build: wires the renderer (Step 14), kit buffer (Step 13), Contour
// destinations (Step 12), feedback network (Steps 10-11), and all four
// processors (Steps 4/5/7/9) into the _NT_algorithm/_NT_factory structure
// with the interim Play/loop player and the real performer parameters.
//
// See 4x4-System-Overview.md for the full design. This file is the
// integration layer only — all DSP lives in the headers it includes.
//
// ---- Step 15 decisions (the plan asked these to be settled here) ----
//
// 1. OUTPUT BUS gets a paired Output Mode (Add/Replace) parameter, via
//    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE — resolving §10's open question in
//    favor of following the API's own convention (every audio output in
//    this project's reference examples uses _WITH_MODE). Add mode lets the
//    kit sum onto a bus; Replace overwrites it. Costs one extra parameter,
//    which is cheap and idiomatic.
//
// 2. CONTOUR SHARED SETTINGS BANK (windows/chaos/radius/round/hold) is
//    COMPILED-IN CONSTANTS for this interim build, not real parameters —
//    resolving §9's open question for now in favor of the lower-risk
//    choice. Rationale: it keeps this first loadable build's parameter set
//    focused on what the plan explicitly lists; it sidesteps the
//    mid-render reconfiguration churn that live-editable Contour settings
//    would require (each change rebuilds all 30 engines); and §9 frames the
//    bank as "set once, applied identically everywhere," which compiled
//    defaults satisfy exactly. They can be promoted to real parameters
//    later without any architectural change (Renderer already takes a
//    SharedSettings in initialise()). Documented as interim in §9.
//
// 3. REGENERATE is a momentary trigger parameter (enum Hold/Regenerate):
//    setting it to Regenerate kicks off a fresh kit bake, then auto-resets
//    to Hold via NT_setParameterFromAudio so it behaves like a button.
//    §9 anticipated "triggering a new render" as a likely real parameter.
//
// 4. P1/P2 DEPTH are 0-100% performer SCALES on the Contour-driven depth
//    (§2/§3: the parameter scales the curve's reach, the curve supplies the
//    motion, the randomized base supplies the center) — not a second
//    independent depth value.
//
// 5. PLAY=Off outputs silence (§6's stated assumption, confirmed here).
//
// No dynamic allocation, no STL, no exceptions/RTTI. The large Renderer +
// double kit buffer live in the firmware-provided DRAM block (placement
// new); the small algorithm instance lives in SRAM.
#include "distingnt/api.h"
#include "fft.h"
#include "renderer.h"
#include "kit_buffer.h"
#include <new>
#include <cmath>
#include <cstdint>

// ===========================================================================
// Spectral slice looper ("Content Filter"), 2026-10-24. Playback-only; lives
// entirely in this file per the workflow rule. When CF ON=1 it replaces the
// normal loop/time-stretch player: it splits the current play buffer into 1024
// base slices, measures 8 band energies per slice (streaming SVF bank, chunked
// so CPU stays flat), merges them at the "Slices" resolution, computes a
// spectral centroid per merged slice, and loops the slice whose centroid is
// nearest "Cutoff" (with hysteresis + equal-power crossfades). The analysis
// runs on kit.current() after each render swap; until the new map is ready it
// HOLDS the last looped slice (user choice). CF OFF = unchanged behavior.
// ===========================================================================
namespace sliceloop {

static const int kBaseSlices = 1024;
static const int kBands      = 8;
static const int kNudgeWin   = 64;     // zero-crossing search half-window (samples)
static const int kAnalyzeChunk = 512;  // kit samples analysed per step() (flat CPU)
static const float kSilenceFrac = 0.02f;   // slices below this fraction of the loudest are skipped

// 8 octave-spaced band centres (Hz), ~80 Hz .. 10.24 kHz (inside the 32 kHz kit).
static const float kBandHz[kBands] = { 80.f, 160.f, 320.f, 640.f, 1280.f, 2560.f, 5120.f, 10240.f };
// "Slices" enum -> number of merged slices.
static const int kSlicesOptions[7] = { 1024, 512, 256, 128, 64, 32, 16 };

inline float clampf( float v, float lo, float hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }

// Interpolated read of the kit (same convention as the normal player).
inline float kitInterp( const kitbuffer::KitBuffer& kb, double pos )
{
    const int N = kitbuffer::kNumSamples;
    int i0 = (int)pos; if ( i0 < 0 ) i0 = 0; if ( i0 >= N ) i0 = N - 1;
    int i1 = i0 + 1; if ( i1 >= N ) i1 = 0;
    float frac = (float)( pos - (double)i0 );
    return kb.read( i0 ) * ( 1.0f - frac ) + kb.read( i1 ) * frac;
}

// A single looping voice over [start, start+loopLen), with an equal-power seam
// crossfade so the loop has no click, read with per-sample advance `adv`.
struct LoopVoice
{
    double start = 0.0, loopLen = 1.0, phase = 0.0, fadeLen = 0.0;

    float read( const kitbuffer::KitBuffer& kb, double adv )
    {
        float main = kitInterp( kb, start + phase );
        float out = main;
        if ( fadeLen > 0.0 && phase >= loopLen - fadeLen )
        {
            double headPhase = phase - ( loopLen - fadeLen );     // 0..fadeLen
            float head = kitInterp( kb, start + headPhase );
            float t = (float)( headPhase / fadeLen );             // 0..1
            const float kHalfPi = 1.57079633f;
            float gOut = cosf( t * kHalfPi ), gHead = sinf( t * kHalfPi );
            out = gOut * main + gHead * head;                     // equal-power tail->head
        }
        phase += adv;
        while ( phase >= loopLen ) phase -= loopLen;
        return out;
    }
};

// Streaming 8-band energy analyzer for one buffer, chunked across step()s.
class Analyzer
{
public:
    void begin()
    {
        float fs = (float)kitbuffer::kSampleRateHz;
        const float kPi = 3.14159265f, k = 0.70710678f;   // Q ~ 1.41 (octave-ish)
        for ( int b = 0; b < kBands; ++b )
        {
            float g = tanf( kPi * kBandHz[b] / fs );
            a1_[b] = 1.0f / ( 1.0f + g * ( g + k ) );
            a2_[b] = g * a1_[b];
            a3_[b] = g * a2_[b];
            ic1_[b] = ic2_[b] = 0.0f;
            acc_[b] = 0.0f;
        }
        pos_ = 0; slice_ = 0; done_ = false;
        nominalNext_ = sliceEnd( 0 );
        computeNudge( 0 );   // slice 0 start nudge
    }

    bool done() const { return done_; }

    // Process up to kAnalyzeChunk samples; store finished base slices into map.
    // Returns true once the whole buffer is analysed.
    bool chunk( const kitbuffer::KitBuffer& kb, uint16_t energy[kBaseSlices][kBands], int8_t nudge[kBaseSlices] )
    {
        if ( done_ ) return true;
        const int N = kitbuffer::kNumSamples;
        int processed = 0;
        while ( pos_ < N && processed < kAnalyzeChunk )
        {
            float x = kb.read( pos_ );
            for ( int b = 0; b < kBands; ++b )
            {
                float v3 = x - ic2_[b];
                float v1 = a1_[b] * ic1_[b] + a2_[b] * v3;
                float v2 = ic2_[b] + a2_[b] * ic1_[b] + a3_[b] * v3;
                ic1_[b] = 2.0f * v1 - ic1_[b];
                ic2_[b] = 2.0f * v2 - ic2_[b];
                acc_[b] += v1 * v1;                     // band energy (bandpass^2)
            }
            ++pos_; ++processed;
            if ( pos_ >= nominalNext_ )                 // finished base slice slice_
            {
                finishSlice( slice_, energy );
                nudge[slice_] = pendingNudge_;
                ++slice_;
                if ( slice_ >= kBaseSlices ) { done_ = true; break; }
                nominalNext_ = sliceEnd( slice_ );
                computeNudge( slice_ );
            }
        }
        if ( pos_ >= N && !done_ )                      // flush the last partial slice
        {
            finishSlice( slice_, energy );
            nudge[slice_] = pendingNudge_;
            for ( int s = slice_ + 1; s < kBaseSlices; ++s )
            { for ( int b = 0; b < kBands; ++b ) energy[s][b] = 0; nudge[s] = 0; }
            done_ = true;
        }
        return done_;
    }

    static int sliceStart( int i ) { return (int)( (int64_t)i * kitbuffer::kNumSamples / kBaseSlices ); }

private:
    static int sliceEnd( int i ) { return sliceStart( i + 1 ); }

    void finishSlice( int i, uint16_t energy[kBaseSlices][kBands] )
    {
        int len = sliceEnd( i ) - sliceStart( i ); if ( len < 1 ) len = 1;
        // meanSquare per band, referenced to 11V^2 full-scale -> uint16.
        float scale = 65535.0f / ( (float)len * 121.0f );
        for ( int b = 0; b < kBands; ++b )
        {
            float e = acc_[b] * scale;
            energy[i][b] = (uint16_t)clampf( e, 0.0f, 65535.0f );
            acc_[b] = 0.0f;
        }
    }

    // Nearest zero crossing to the nominal start of slice i, within +-kNudgeWin.
    void computeNudge( int i )
    {
        const int N = kitbuffer::kNumSamples;
        int nom = sliceStart( i );
        pendingNudge_ = 0;
        if ( nom <= 0 || nom >= N - 1 ) return;
        for ( int d = 0; d <= kNudgeWin; ++d )
        {
            int p = nom + d;
            if ( p >= 1 && p < N && signChange( kb_, p ) ) { pendingNudge_ = clampNudge( d ); return; }
            p = nom - d;
            if ( p >= 1 && p < N && signChange( kb_, p ) ) { pendingNudge_ = clampNudge( -d ); return; }
        }
    }
    static int8_t clampNudge( int d ) { if ( d > 127 ) d = 127; if ( d < -128 ) d = -128; return (int8_t)d; }

    // kb_ is set by chunk() each call so computeNudge (called from begin/chunk) can read.
public:
    void setBuffer( const kitbuffer::KitBuffer* kb ) { kb_ = kb; }
private:
    static bool signChange( const kitbuffer::KitBuffer* kb, int p )
    {
        float a = kb->read( p - 1 ), b = kb->read( p );
        return ( a < 0.0f ) != ( b < 0.0f );
    }

    const kitbuffer::KitBuffer* kb_ = nullptr;
    float a1_[kBands], a2_[kBands], a3_[kBands], ic1_[kBands], ic2_[kBands], acc_[kBands];
    int   pos_ = 0, slice_ = 0, nominalNext_ = 0;
    int8_t pendingNudge_ = 0;
    bool  done_ = true;
};

// Cutoff knob (0..1) -> target centroid frequency, log 20..20000 Hz.
inline float cutoffHz( float c01 ) { return 20.0f * expf( c01 * 6.9077553f ); }

// -----------------------------------------------------------------------------
// SliceAnalysis — SHARED per-buffer analysis (lives in DRAM). Built once per
// render from kit.current(); the merged map (at the global "Slices" grain) is
// sorted by centroid so each player can pick a slice by Cutoff + a rank offset.
// -----------------------------------------------------------------------------
class SliceAnalysis
{
public:
    SliceAnalysis() { for ( int b = 0; b < kBands; ++b ) logBand_[b] = logf( kBandHz[b] ); reset(); }

    void reset() { ready_ = false; lastSlicesIdx_ = -1; mergedCount_ = 0; validCount_ = 0; }
    bool ready() const { return ready_; }

    void onBufferChanged( const kitbuffer::KitBuffer& kb )
    {
        ready_ = false; lastSlicesIdx_ = -1;
        analyzer_.setBuffer( &kb );
        analyzer_.begin();
    }

    void analyzeChunk( const kitbuffer::KitBuffer& kb )
    {
        if ( ready_ ) return;
        analyzer_.setBuffer( &kb );
        if ( analyzer_.chunk( kb, energy_, nudge_ ) ) ready_ = true;
    }

    // Rebuild the merged map + sorted order when the global Slices grain changes
    // (or the first time after analysis completes). Cheap; call once per block.
    void rebuildIfNeeded( int slicesIdx )
    {
        if ( !ready_ ) return;
        if ( slicesIdx < 0 ) slicesIdx = 0;
        if ( slicesIdx > 6 ) slicesIdx = 6;
        if ( slicesIdx == lastSlicesIdx_ ) return;
        lastSlicesIdx_ = slicesIdx;
        rebuildMerged( slicesIdx );
        sortValidByCentroid();
    }

    bool hasValid() const { return validCount_ > 0; }

    // Pick a slice by Cutoff (nearest centroid) stepped by a rank offset derived
    // from startPos01 (0 = nearest to Cutoff, up = progressively brighter).
    // Returns the slice's zero-crossing start sample and its centroid.
    bool select( float cutoff01, float startPos01, double& outStartSample, float& outCentroid ) const
    {
        if ( validCount_ <= 0 ) return false;
        float target = logf( cutoffHz( cutoff01 ) );
        // binary search for the first centroid >= target (ascending order)
        int lo = 0, hi = validCount_ - 1, ins = validCount_;
        while ( lo <= hi )
        {
            int mid = ( lo + hi ) >> 1;
            if ( mergedCentroid_[sortedIdx_[mid]] >= target ) { ins = mid; hi = mid - 1; }
            else lo = mid + 1;
        }
        int nearest;
        if ( ins <= 0 ) nearest = 0;
        else if ( ins >= validCount_ ) nearest = validCount_ - 1;
        else
        {
            float dHi = fabsf( mergedCentroid_[sortedIdx_[ins]] - target );
            float dLo = fabsf( mergedCentroid_[sortedIdx_[ins - 1]] - target );
            nearest = ( dLo <= dHi ) ? ins - 1 : ins;
        }
        int step = (int)( startPos01 * (float)( validCount_ - 1 ) + 0.5f );
        int rank = nearest + step;
        if ( rank < 0 ) rank = 0;
        if ( rank > validCount_ - 1 ) rank = validCount_ - 1;
        int m = sortedIdx_[rank];
        int base = mergedStartBase_[m];
        outStartSample = (double)( Analyzer::sliceStart( base ) + nudge_[base] );
        outCentroid = mergedCentroid_[m];
        return true;
    }

private:
    void rebuildMerged( int slicesIdx )
    {
        int count = kSlicesOptions[slicesIdx];
        int group = kBaseSlices / count;
        mergedCount_ = count;
        float maxTotal = 0.0f;
        for ( int m = 0; m < count; ++m )
        {
            uint32_t sum[kBands]; for ( int b = 0; b < kBands; ++b ) sum[b] = 0;
            int base0 = m * group;
            for ( int j = 0; j < group; ++j )
                for ( int b = 0; b < kBands; ++b ) sum[b] += energy_[base0 + j][b];
            float num = 0.0f, den = 0.0f;
            for ( int b = 0; b < kBands; ++b ) { float e = (float)sum[b]; num += e * logBand_[b]; den += e; }
            mergedStartBase_[m] = (uint16_t)base0;
            mergedCentroid_[m] = ( den > 0.0f ) ? ( num / den ) : -1.0f;
            if ( den > maxTotal ) maxTotal = den;
        }
        float thr = maxTotal * kSilenceFrac;
        for ( int m = 0; m < count; ++m )
        {
            if ( mergedCentroid_[m] < 0.0f ) continue;
            int base0 = m * group; float den = 0.0f;
            for ( int j = 0; j < group; ++j )
                for ( int b = 0; b < kBands; ++b ) den += (float)energy_[base0 + j][b];
            if ( den < thr ) mergedCentroid_[m] = -1.0f;   // near-silent -> skip
        }
    }

    // Collect valid (non-silent) merged indices and heapsort by centroid asc.
    void sortValidByCentroid()
    {
        validCount_ = 0;
        for ( int m = 0; m < mergedCount_; ++m )
            if ( mergedCentroid_[m] >= 0.0f ) sortedIdx_[validCount_++] = (uint16_t)m;
        int nn = validCount_;
        for ( int i = nn / 2 - 1; i >= 0; --i ) siftDown( i, nn );
        for ( int end = nn - 1; end > 0; --end )
        {
            uint16_t t = sortedIdx_[0]; sortedIdx_[0] = sortedIdx_[end]; sortedIdx_[end] = t;
            siftDown( 0, end );
        }
    }
    void siftDown( int root, int n )
    {
        while ( true )
        {
            int child = 2 * root + 1; if ( child >= n ) break;
            if ( child + 1 < n && mergedCentroid_[sortedIdx_[child + 1]] > mergedCentroid_[sortedIdx_[child]] ) ++child;
            if ( mergedCentroid_[sortedIdx_[child]] <= mergedCentroid_[sortedIdx_[root]] ) break;
            uint16_t t = sortedIdx_[root]; sortedIdx_[root] = sortedIdx_[child]; sortedIdx_[child] = t;
            root = child;
        }
    }

    Analyzer analyzer_;
    uint16_t energy_[kBaseSlices][kBands];   // per base slice band energy
    int8_t   nudge_[kBaseSlices];            // per base slice zero-crossing start nudge
    float    mergedCentroid_[kBaseSlices];   // per merged slice log-centroid (-1 = silent)
    uint16_t mergedStartBase_[kBaseSlices];  // per merged slice: first base-slice index
    uint16_t sortedIdx_[kBaseSlices];        // valid merged indices, sorted by centroid asc
    float    logBand_[kBands];
    int      mergedCount_ = 0, validCount_ = 0, lastSlicesIdx_ = -1;
    bool     ready_ = false;
};

// -----------------------------------------------------------------------------
// SlicePlayer — PER-PLAYER CF playback (lives in SRAM). Selects + loops a slice
// from the shared SliceAnalysis; equal-power crossfade on seam and switches.
// Returns raw samples; the caller applies the envelope and normalize gain.
// -----------------------------------------------------------------------------
class SlicePlayer
{
public:
    SlicePlayer() { reset(); }
    void reset() { haveCur_ = false; switching_ = false; curStart_ = -1.0; curCentroid_ = 0.0f; }
    void retrigger() { cur_.phase = 0.0; switching_ = false; }   // note-on: restart the loop

    void selectPerBlock( const SliceAnalysis& an, float cutoff01, float startPos01,
                         float hystSemis, float sliceLenMs, float crossfadeMs, float sampleRate )
    {
        if ( !an.ready() || !an.hasValid() ) return;   // hold whatever we have
        double startS; float cen;
        if ( !an.select( cutoff01, startPos01, startS, cen ) ) return;

        float target = logf( cutoffHz( cutoff01 ) );
        float hystLog = hystSemis * ( 0.69314718f / 12.0f );
        if ( !haveCur_ )
        {
            setVoice( cur_, startS, sliceLenMs, crossfadeMs );
            curStart_ = startS; curCentroid_ = cen; haveCur_ = true; return;
        }
        if ( startS == curStart_ ) { refreshVoiceLen( cur_, sliceLenMs, crossfadeMs ); return; }
        float candD = fabsf( cen - target ), curD = fabsf( curCentroid_ - target );
        if ( candD < curD - hystLog )
        {
            old_ = cur_; switching_ = true; switchPhase_ = 0.0;
            switchLen_ = (double)crossfadeMs * 0.001 * (double)sampleRate;
            if ( switchLen_ < 1.0 ) switchLen_ = 1.0;
            setVoice( cur_, startS, sliceLenMs, crossfadeMs );
            curStart_ = startS; curCentroid_ = cen;
        }
        else refreshVoiceLen( cur_, sliceLenMs, crossfadeMs );
    }

    float processSample( const kitbuffer::KitBuffer& kb, double adv )
    {
        if ( !haveCur_ ) return 0.0f;
        float s = cur_.read( kb, adv );
        if ( switching_ )
        {
            float o = old_.read( kb, adv );
            float t = (float)( switchPhase_ / switchLen_ ); if ( t > 1.0f ) t = 1.0f;
            const float kHalfPi = 1.57079633f;
            s = sinf( t * kHalfPi ) * s + cosf( t * kHalfPi ) * o;
            switchPhase_ += 1.0;
            if ( switchPhase_ >= switchLen_ ) switching_ = false;
        }
        return s;
    }

private:
    void setVoice( LoopVoice& v, double startS, float sliceLenMs, float crossfadeMs )
    {
        v.start = startS; v.phase = 0.0;
        refreshVoiceLen( v, sliceLenMs, crossfadeMs );
    }
    void refreshVoiceLen( LoopVoice& v, float sliceLenMs, float crossfadeMs )
    {
        double len = (double)sliceLenMs * 0.001 * (double)kitbuffer::kSampleRateHz;
        if ( len < 2.0 ) len = 2.0;
        double maxLen = (double)kitbuffer::kNumSamples - v.start - 1.0;
        if ( len > maxLen ) len = maxLen;
        v.loopLen = len;
        double f = (double)crossfadeMs * 0.001 * (double)kitbuffer::kSampleRateHz;
        if ( f > len * 0.5 ) f = len * 0.5;
        if ( f < 0.0 ) f = 0.0;
        v.fadeLen = f;
        if ( v.phase >= v.loopLen ) v.phase = 0.0;
    }

    LoopVoice cur_, old_;
    double curStart_ = -1.0, switchPhase_ = 0.0, switchLen_ = 1.0;
    float  curCentroid_ = 0.0f;
    bool   haveCur_ = false, switching_ = false;
};

} // namespace sliceloop

// Everything large, placed into the DRAM block the firmware provides.
struct Engine
{
    renderer::Renderer         renderer;
    kitbuffer::DoubleBufferedKit kit;
    sliceloop::SliceAnalysis   sliceAnalysis;   // shared per-buffer slice map (4 players read it)
};

// Compile-time budget guard: the firmware's DRAM pool is 512KB, shared with
// every other algorithm in the preset. This instrument is designed to use
// most of it (§8). CRITICAL (2026-10-20 regression fix): the req.dram Engine
// is NOT the only RAM this plugin consumes — the .o's own static `.bss`
// (~8KB, mostly fft.h's g_twiddle table) must ALSO be placed in RAM by the
// loader. The earlier guard only checked sizeof(Engine) and ignored that,
// letting the real total (bss + Engine) overrun the 512KB pool, which took
// the plugin silent on hardware. This reserve (16KB) covers the static
// footprint plus headroom so static + dynamic stays safely under 512KB.
static_assert( sizeof( Engine ) <= 512 * 1024 - 16384,
               "Engine + ~8KB static .bss would exceed the 512KB DRAM pool — trim the kit (kit_buffer.h). See the 2026-10-20 regression fix." );

// One note-gated voice (4 of them, all reading the shared kit buffer). Small
// state only -> lives in SRAM with the algorithm instance. Each has a gated AHR
// volume envelope plus its own stretch read head and CF slice-looper state.
struct Player
{
    enum { kIdle = 0, kAttack = 1, kSustain = 2, kRelease = 3 };
    int    envState;
    float  envCounter, envOut, relLevel;
    bool   gate;    // MIDI note held (set by midiMessage)
    bool   trig;    // pending note-on edge (consumed by step)
    timestretch::StretchReader reader;   // CF-off (time-stretch) playback
    sliceloop::SlicePlayer     slice;    // CF-on (content filter) playback

    Player() : envState(kIdle), envCounter(0.f), envOut(0.f), relLevel(0.f), gate(false), trig(false) {}
    void noteOn()  { gate = true; trig = true; }
    void noteOff() { gate = false; }

    // Gated AHR: attack -> sustain(100% while held) -> release on note-off.
    // Fixed power curve (exp 3), like AHRVCA but with gate-length hold.
    float envProcess( float atkSamp, float relSamp )
    {
        const float e = 3.0f;
        if ( ( envState == kAttack || envState == kSustain ) && !gate )
        { envState = kRelease; envCounter = 0.f; relLevel = envOut; }
        switch ( envState )
        {
        case kAttack: {
            float t = atkSamp > 0.f ? envCounter / atkSamp : 1.f;
            if ( t >= 1.f ) { envState = kSustain; envCounter = 0.f; envOut = 1.f; }
            else { envOut = 1.f - powf( 1.f - t, e ); envCounter += 1.f; }
            break; }
        case kSustain: envOut = 1.f; break;
        case kRelease: {
            float t = relSamp > 0.f ? envCounter / relSamp : 1.f;
            if ( t >= 1.f ) { envState = kIdle; envOut = 0.f; }
            else { envOut = relLevel * powf( 1.f - t, e ); envCounter += 1.f; }
            break; }
        default: envOut = 0.f; break;
        }
        return envOut;
    }
};

struct FourByFourAlg : public _NT_algorithm
{
    Engine* engine;
    Player   players[4];
    uint32_t lastSwapGen;   // last seen renderer swap generation
    uint32_t sliceGen;      // swap generation the shared slice analysis was (re)started for
    int      prevRegen;     // previous Regenerate value, for 0->1 edge detection in step()

    // Initialize only our OWN members. Deliberately do NOT touch the base's
    // v / vIncludingCommon — those are system-managed (api.h).
    FourByFourAlg() : engine( nullptr ), lastSwapGen( 0 ), sliceGen( 0xFFFFFFFFu ), prevRegen( 0 ) {}
};

// Fixed base seed for reproducibility (§10's seed-derived-vs-entropy
// question stays open; a fixed constant is the reproducible choice, and the
// renderIndex increments per render so successive bakes still differ).
static const uint32_t kBaseSeed = 0x34783446u;   // "4x4F"

// Per-player field order (used both to lay out the 32 player params and to
// index them at runtime). playerParam(p,f) = kParamPlayerBase + p*8 + f.
enum { PF_StartPos = 0, PF_Pitch, PF_Attack, PF_Release, PF_Stretch, PF_CF, PF_Cutoff, PF_SliceLen, PF_COUNT };

enum {
    // RENDER (sticky switches; Regenerate fires on 0->1, both positions stick)
    kParamRegenerate, kParamPlay,
    // STARTER (randomization ranges; freq/length/waveform randomized per render)
    kParamMinFreq, kParamMaxFreq, kParamMinLen, kParamMaxLen,
    // DEPTH (Contour modulation amount per processor)
    kParamShifterDepth, kParamFreqDepth, kParamDelayDepth, kParamPhaserDepth,
    // CONTOUR
    kParamWindows, kParamChaos, kParamRadius, kParamRoundProb, kParamHoldProb,
    // FILTERS
    kParamBpFreq1, kParamBpFreq2, kParamBpFreq3, kParamBpFreq4,
    kParamBpBw1, kParamBpBw2, kParamBpBw3, kParamBpBw4,
    kParamVcfRnd, kParamQCont,
    // SHIFTER (P1 spectral manipulation)
    kParamBlockSize, kParamSwaps, kParamSwapSize, kParamOmit,
    // PLAYERS 1..4 — 32 params (8 per player, field order PF_*)
    kParamPlayerBase,
    // REST (globals shared by all players)
    kParamThreshold = kParamPlayerBase + 4 * PF_COUNT,
    kParamXfade, kParamSlices, kParamHyst, kParamCrossfade,
    // ROUTING
    kParamOutput, kParamOutputMode,
    kParamNote1, kParamNote2, kParamNote3, kParamNote4,
    kNumParams
};

inline int playerParam( int p, int f ) { return kParamPlayerBase + p * PF_COUNT + f; }

static char const* const regenStrings[] = { "Hold", "Regenerate", NULL };
static char const* const playStrings[]  = { "Off", "On", NULL };
static char const* const blockSizeStrings[] = { "1", "2", "4", "8", "16", "32", "64", NULL };
static const int kBlockSizeBins[] = { 1, 2, 4, 8, 16, 32, 64 };   // enum index -> bins
static char const* const slicesStrings[] = { "1024", "512", "256", "128", "64", "32", "16", NULL };

// 8 player params per player; CF Cutoff uses kNT_unitHasStrings for a Hz readout.
#define PLAYER_PARAMS(N) \
    { .name = "P" #N " StartPos", .min=0,   .max=100,  .def=0,   .unit=kNT_unitPercent,    .scaling=0,              .enumStrings=NULL }, \
    { .name = "P" #N " Pitch",    .min=-48, .max=48,   .def=0,   .unit=kNT_unitSemitones,  .scaling=0,              .enumStrings=NULL }, \
    { .name = "P" #N " Attack",   .min=1,   .max=500,  .def=5,   .unit=kNT_unitMs,         .scaling=0,              .enumStrings=NULL }, \
    { .name = "P" #N " Release",  .min=1,   .max=1000, .def=150, .unit=kNT_unitMs,         .scaling=0,              .enumStrings=NULL }, \
    { .name = "P" #N " Stretch",  .min=100, .max=800,  .def=100, .unit=kNT_unitNone,       .scaling=kNT_scaling100, .enumStrings=NULL }, \
    { .name = "P" #N " CF",       .min=0,   .max=1,    .def=0,   .unit=kNT_unitEnum,       .scaling=0,              .enumStrings=playStrings }, \
    { .name = "P" #N " Cutoff",   .min=0,   .max=1000, .def=500, .unit=kNT_unitHasStrings, .scaling=0,              .enumStrings=NULL }, \
    { .name = "P" #N " SliceLen", .min=1,   .max=1000, .def=100, .unit=kNT_unitMs,         .scaling=0,              .enumStrings=NULL },

static const _NT_parameter parameters[] = {
    // RENDER
    { .name = "Regenerate", .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=regenStrings },  // sticky; render on 0->1
    { .name = "Play",       .min=0, .max=1, .def=1, .unit=kNT_unitEnum, .scaling=0, .enumStrings=playStrings },   // master enable (Off = silence)
    // STARTER (randomized per render: freq in [Min,Max]Hz, length in [Min,Max]ms, waveform sq/saw/tri/noise)
    { .name = "MinFreq",   .min=20, .max=2000, .def=50,   .unit=kNT_unitHz, .scaling=0, .enumStrings=NULL },
    { .name = "MaxFreq",   .min=20, .max=2000, .def=500,  .unit=kNT_unitHz, .scaling=0, .enumStrings=NULL },
    { .name = "MinLength", .min=100,.max=1000, .def=100,  .unit=kNT_unitMs, .scaling=0, .enumStrings=NULL },      // 0.1 .. 1 s
    { .name = "MaxLength", .min=100,.max=1000, .def=1000, .unit=kNT_unitMs, .scaling=0, .enumStrings=NULL },
    // DEPTH
    { .name = "ShifterDepth", .min=0, .max=100, .def=100, .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name = "FreqDepth",    .min=0, .max=100, .def=100, .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name = "DelayDepth",   .min=0, .max=100, .def=100, .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name = "PhaserDepth",  .min=0, .max=100, .def=100, .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    // CONTOUR
    { .name = "Windows",    .min=1, .max=32,  .def=6,   .unit=kNT_unitNone,    .scaling=0, .enumStrings=NULL },
    { .name = "Chaos",      .min=0, .max=100, .def=60,  .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name = "Radius",     .min=0, .max=500, .def=120, .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    { .name = "Round prob", .min=0, .max=100, .def=100, .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name = "Hold prob",  .min=0, .max=100, .def=0,   .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    // FILTERS
    { .name = "Freq 1", .min=20, .max=16000, .def=200,  .unit=kNT_unitHz, .scaling=0, .enumStrings=NULL },
    { .name = "Freq 2", .min=20, .max=16000, .def=600,  .unit=kNT_unitHz, .scaling=0, .enumStrings=NULL },
    { .name = "Freq 3", .min=20, .max=16000, .def=1500, .unit=kNT_unitHz, .scaling=0, .enumStrings=NULL },
    { .name = "Freq 4", .min=20, .max=16000, .def=4000, .unit=kNT_unitHz, .scaling=0, .enumStrings=NULL },
    { .name = "BW 1", .min=1, .max=96, .def=48, .unit=kNT_unitSemitones, .scaling=0, .enumStrings=NULL },
    { .name = "BW 2", .min=1, .max=96, .def=48, .unit=kNT_unitSemitones, .scaling=0, .enumStrings=NULL },
    { .name = "BW 3", .min=1, .max=96, .def=48, .unit=kNT_unitSemitones, .scaling=0, .enumStrings=NULL },
    { .name = "BW 4", .min=1, .max=96, .def=48, .unit=kNT_unitSemitones, .scaling=0, .enumStrings=NULL },
    { .name = "VCFRND", .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=playStrings },
    { .name = "QCONT",  .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=playStrings },
    // SHIFTER
    { .name = "BlockSize", .min=0, .max=6,   .def=3, .unit=kNT_unitEnum,    .scaling=0, .enumStrings=blockSizeStrings },
    { .name = "Swaps",     .min=0, .max=100, .def=0, .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name = "SwapSize",  .min=0, .max=100, .def=50,.unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name = "Omit",      .min=0, .max=100, .def=0, .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    // PLAYERS 1..4 (8 each)
    PLAYER_PARAMS(1)
    PLAYER_PARAMS(2)
    PLAYER_PARAMS(3)
    PLAYER_PARAMS(4)
    // REST (globals)
    { .name = "Threshold", .min=0, .max=100, .def=30, .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name = "Xfade",     .min=1, .max=50,  .def=10, .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    { .name = "Slices",    .min=0, .max=6,   .def=0,  .unit=kNT_unitEnum,    .scaling=0, .enumStrings=slicesStrings },
    { .name = "Hysteresis",.min=0, .max=12,  .def=1,  .unit=kNT_unitSemitones,.scaling=0,.enumStrings=NULL },
    { .name = "Crossfade", .min=0, .max=100, .def=10, .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    // ROUTING
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output", 1, 13 )
    { .name = "Note 1", .min=0, .max=127, .def=60, .unit=kNT_unitMIDINote, .scaling=0, .enumStrings=NULL },
    { .name = "Note 2", .min=0, .max=127, .def=61, .unit=kNT_unitMIDINote, .scaling=0, .enumStrings=NULL },
    { .name = "Note 3", .min=0, .max=127, .def=62, .unit=kNT_unitMIDINote, .scaling=0, .enumStrings=NULL },
    { .name = "Note 4", .min=0, .max=127, .def=63, .unit=kNT_unitMIDINote, .scaling=0, .enumStrings=NULL },
};

static const uint8_t pageRender[]  = { kParamRegenerate, kParamPlay };
static const uint8_t pageStarter[] = { kParamMinFreq, kParamMaxFreq, kParamMinLen, kParamMaxLen };
static const uint8_t pageDepth[]   = { kParamShifterDepth, kParamFreqDepth, kParamDelayDepth, kParamPhaserDepth };
static const uint8_t pageContour[] = { kParamWindows, kParamChaos, kParamRadius, kParamRoundProb, kParamHoldProb };
static const uint8_t pageFilters[] = { kParamBpFreq1, kParamBpFreq2, kParamBpFreq3, kParamBpFreq4,
                                       kParamBpBw1, kParamBpBw2, kParamBpBw3, kParamBpBw4,
                                       kParamVcfRnd, kParamQCont };
static const uint8_t pageShifter[] = { kParamBlockSize, kParamSwaps, kParamSwapSize, kParamOmit };
#define PLAYER_PAGE(P) { kParamPlayerBase+(P)*PF_COUNT+PF_StartPos, kParamPlayerBase+(P)*PF_COUNT+PF_Pitch, \
                         kParamPlayerBase+(P)*PF_COUNT+PF_Attack,   kParamPlayerBase+(P)*PF_COUNT+PF_Release, \
                         kParamPlayerBase+(P)*PF_COUNT+PF_Stretch,  kParamPlayerBase+(P)*PF_COUNT+PF_CF, \
                         kParamPlayerBase+(P)*PF_COUNT+PF_Cutoff,   kParamPlayerBase+(P)*PF_COUNT+PF_SliceLen }
static const uint8_t pagePlayer1[] = PLAYER_PAGE(0);
static const uint8_t pagePlayer2[] = PLAYER_PAGE(1);
static const uint8_t pagePlayer3[] = PLAYER_PAGE(2);
static const uint8_t pagePlayer4[] = PLAYER_PAGE(3);
static const uint8_t pageRest[]    = { kParamThreshold, kParamXfade, kParamSlices, kParamHyst, kParamCrossfade };
static const uint8_t pageRouting[] = { kParamOutput, kParamOutputMode, kParamNote1, kParamNote2, kParamNote3, kParamNote4 };

static const _NT_parameterPage pages[] = {
    { .name = "Render",   .numParams = ARRAY_SIZE( pageRender ),  .group = 0, .params = pageRender },
    { .name = "Starter",  .numParams = ARRAY_SIZE( pageStarter ), .group = 0, .params = pageStarter },
    { .name = "Depth",    .numParams = ARRAY_SIZE( pageDepth ),   .group = 0, .params = pageDepth },
    { .name = "Contour",  .numParams = ARRAY_SIZE( pageContour ), .group = 0, .params = pageContour },
    { .name = "Filters",  .numParams = ARRAY_SIZE( pageFilters ), .group = 0, .params = pageFilters },
    { .name = "Shifter",  .numParams = ARRAY_SIZE( pageShifter ), .group = 0, .params = pageShifter },
    { .name = "Player 1", .numParams = ARRAY_SIZE( pagePlayer1 ), .group = 0, .params = pagePlayer1 },
    { .name = "Player 2", .numParams = ARRAY_SIZE( pagePlayer2 ), .group = 0, .params = pagePlayer2 },
    { .name = "Player 3", .numParams = ARRAY_SIZE( pagePlayer3 ), .group = 0, .params = pagePlayer3 },
    { .name = "Player 4", .numParams = ARRAY_SIZE( pagePlayer4 ), .group = 0, .params = pagePlayer4 },
    { .name = "REST",     .numParams = ARRAY_SIZE( pageRest ),    .group = 0, .params = pageRest },
    { .name = "Routing",  .numParams = ARRAY_SIZE( pageRouting ), .group = 0, .params = pageRouting },
};

static const _NT_parameterPages parameterPages = {
    .numPages = ARRAY_SIZE( pages ),
    .pages    = pages,
};

// Push all continuously-applied parameter values from v[] into the renderer.
// Used at construct (v[] holds defaults there, per CLAUDE.md) and mirrored
// per-parameter in parameterChanged.
// Push the render-side (bake) parameters into the renderer. The per-player
// playback params (StartPos/Pitch/Attack/Release/Stretch/CF/Cutoff/SliceLen)
// and the globals (Threshold/Xfade/Slices/Hyst/Crossfade, Notes) are read live
// in step(), not here. Oversample(1x), Norm level(8V) and all-processors-on are
// now fixed (their parameters were removed 2026-10-24).
static void applyAllParams( FourByFourAlg* a )
{
    renderer::Renderer& r = a->engine->renderer;
    r.setStarterRanges( (float)a->v[kParamMinFreq], (float)a->v[kParamMaxFreq],
                        (float)a->v[kParamMinLen] * 0.001f, (float)a->v[kParamMaxLen] * 0.001f );
    r.setP1DepthScale( (float)a->v[kParamShifterDepth] / 100.0f );
    r.setP2DepthScale( (float)a->v[kParamFreqDepth]    / 100.0f );
    r.setP3DepthScale( (float)a->v[kParamDelayDepth]   / 100.0f );
    r.setP4DepthScale( (float)a->v[kParamPhaserDepth]  / 100.0f );

    for ( int i = 0; i < 4; ++i )
    {
        r.setBandpassFreq( i, (float)a->v[kParamBpFreq1 + i] );
        r.setBandpassBw( i, (float)a->v[kParamBpBw1 + i] );
    }
    r.setVcfRnd( a->v[kParamVcfRnd] != 0 );
    r.setQCont( a->v[kParamQCont] != 0 );

    {
        int bsIdx = a->v[kParamBlockSize];
        if ( bsIdx < 0 ) bsIdx = 0;
        if ( bsIdx > 6 ) bsIdx = 6;
        r.setP1BlockSize( kBlockSizeBins[bsIdx] );
        r.setP1Swaps( (float)a->v[kParamSwaps] );
        r.setP1SwapSize( (float)a->v[kParamSwapSize] );
        r.setP1Omit( (float)a->v[kParamOmit] );
    }

    contourdest::SharedSettings cs;
    cs.windows   = (int)a->v[kParamWindows];
    cs.chaos     = (float)a->v[kParamChaos] / 100.0f;
    cs.radiusSec = (float)a->v[kParamRadius] / 1000.0f;
    cs.roundProb = (float)a->v[kParamRoundProb] / 100.0f;
    cs.holdProb  = (float)a->v[kParamHoldProb] / 100.0f;
    r.setSharedSettings( cs );

    // Fixed now (parameters removed): 8V playback normalize, 1x oversample,
    // all four processors always on.
    r.setNormalizeLevel( 8.0f );
    r.setOversample( 1 );
    for ( int i = 0; i < 4; ++i ) r.setProcessorEnabled( i, true );
}

// Custom display for each player's Cutoff: a 0..1000 raw knob mapped
// logarithmically to 20..20000 Hz, shown in Hz. 0 for anything else.
int parameterString( _NT_algorithm* /*self*/, int p, int v, char* buff )
{
    bool isCutoff = false;
    for ( int pl = 0; pl < 4; ++pl ) if ( p == playerParam( pl, PF_Cutoff ) ) isCutoff = true;
    if ( !isCutoff ) return 0;
    float hz = 20.0f * expf( (float)v / 1000.0f * 6.9077553f );
    int len = NT_intToString( buff, (int32_t)( hz + 0.5f ) );
    buff[len] = ' '; buff[len + 1] = 'H'; buff[len + 2] = 'z'; buff[len + 3] = 0;
    return len + 3;
}

// MIDI note on/off gates the 4 players (omni; each listens to its Note param).
void midiMessage( _NT_algorithm* self, uint8_t b0, uint8_t b1, uint8_t b2 )
{
    FourByFourAlg* a = (FourByFourAlg*)self;
    int status = b0 & 0xF0;
    bool noteOn  = ( status == 0x90 && b2 > 0 );
    bool noteOff = ( status == 0x80 ) || ( status == 0x90 && b2 == 0 );
    if ( !noteOn && !noteOff ) return;
    int note = b1;
    for ( int p = 0; p < 4; ++p )
    {
        if ( note == a->v[kParamNote1 + p] )
        {
            if ( noteOn ) a->players[p].noteOn();
            else          a->players[p].noteOff();
        }
    }
}

void calculateRequirements( _NT_algorithmRequirements& req, const int32_t* /*specifications*/ )
{
    req.numParameters = ARRAY_SIZE( parameters );
    req.sram = sizeof( FourByFourAlg );
    req.dram = sizeof( Engine );
    req.dtc  = 0;
    req.itc  = 0;
}

_NT_algorithm* construct( const _NT_algorithmMemoryPtrs& ptrs,
                           const _NT_algorithmRequirements& /*req*/,
                           const int32_t* /*specifications*/ )
{
    // Required before any P1 (phase-vocoder) use; idempotent, cooperative
    // single-threaded step() can't interleave with this (see p1_pitchshift.h).
    fft::initTwiddles();

    FourByFourAlg* a = new ( ptrs.sram ) FourByFourAlg();
    a->engine = new ( ptrs.dram ) Engine();
    a->parameters     = parameters;
    a->parameterPages = &parameterPages;

    // NOTE: v[] is deliberately NOT read here. Whether the system populates
    // v before or after construct() is ambiguous across the examples, so we
    // don't depend on it. The renderer's internal defaults already match the
    // parameter defaults exactly (starter 110Hz/200ms, depths 100%), so a
    // default load bakes its first kit correctly; step() then syncs live v[]
    // (applyAllParams) every call, so a preset's non-default values take
    // effect from the next render on — see step().
    a->engine->renderer.initialise( kBaseSeed, (float)NT_globals.sampleRate );
    a->engine->renderer.startNewRender();  // begin baking the first kit immediately
    return a;
}

void step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
    FourByFourAlg* a = (FourByFourAlg*)self;

    // Keep the renderer's continuous params in sync with live v[] (cheap).
    // Read live every step() — not cached in a parameterChanged callback —
    // so this is robust to preset loads and to any firmware that doesn't
    // route a change through parameterChanged (the monosynth example relies
    // on the same "read v[] live" pattern, with parameterChanged = NULL).
    applyAllParams( a );

    // Regenerate: STICKY 0/1 switch (potentiometer, not a button). A fresh bake
    // fires on the 0->1 edge; the value stays where the user leaves it.
    int regen = a->v[kParamRegenerate];
    if ( regen == 1 && a->prevRegen == 0 ) a->engine->renderer.startNewRender();
    a->prevRegen = regen;

    // Background render: advance one bounded chunk per step() call (§7).
    a->engine->renderer.advanceChunk( a->engine->kit );

    int n = numFramesBy4 * 4;
    int outBus = a->v[kParamOutput];
    if ( outBus < 1 ) return;
    float* out = busFrames + ( outBus - 1 ) * n;
    bool replace = a->v[kParamOutputMode];
    bool play    = a->v[kParamPlay];
    if ( !play )
    {
        if ( replace ) for ( int i = 0; i < n; ++i ) out[i] = 0.0f;   // master off -> silence
        return;
    }

    const kitbuffer::KitBuffer& kb = a->engine->kit.current();
    const timestretch::PeriodMaps& maps = a->engine->renderer.currentMaps( a->engine->kit );
    float normGain = a->engine->renderer.playbackGain();   // 8V / kit peak
    const float sr = (float)NT_globals.sampleRate;
    const double kitConv = (double)kitbuffer::kSampleRateHz / (double)sr;

    // Shared slice analysis (used by any CF-ON player). Re-analyse kit.current()
    // after each render swap, chunked; rebuild the merged map at the global
    // "Slices" grain. All 4 players read this one map.
    uint32_t gen = a->engine->renderer.swapGeneration();
    a->lastSwapGen = gen;
    bool anyCF = false;
    for ( int p = 0; p < 4; ++p ) if ( a->v[playerParam( p, PF_CF )] != 0 ) anyCF = true;
    if ( anyCF )
    {
        if ( a->sliceGen != gen ) { a->engine->sliceAnalysis.onBufferChanged( kb ); a->sliceGen = gen; }
        a->engine->sliceAnalysis.analyzeChunk( kb );
        a->engine->sliceAnalysis.rebuildIfNeeded( a->v[kParamSlices] );
    }

    // Global (REST page) stretch/CF settings shared by all players.
    float gThreshold01 = (float)a->v[kParamThreshold] / 100.0f;
    float gXfadeMs      = (float)a->v[kParamXfade];
    float gHystSemis    = (float)a->v[kParamHyst];
    float gCrossfadeMs  = (float)a->v[kParamCrossfade];

    // Per-player per-block setup.
    double            advP[4];
    float             atkSamp[4], relSamp[4];
    bool              cfP[4], audible[4];
    timestretch::Params spP[4];
    for ( int p = 0; p < 4; ++p )
    {
        Player& pl = a->players[p];
        float startPos01 = (float)a->v[playerParam( p, PF_StartPos )] / 100.0f;
        float pitchSemis = (float)a->v[playerParam( p, PF_Pitch )];
        float stretchVal = (float)a->v[playerParam( p, PF_Stretch )] / 100.0f;   // 1.00..8.00
        float sliceLenMs = (float)a->v[playerParam( p, PF_SliceLen )];
        cfP[p]  = ( a->v[playerParam( p, PF_CF )] != 0 );
        advP[p] = kitConv * pow( 2.0, (double)pitchSemis / 12.0 );
        atkSamp[p] = (float)a->v[playerParam( p, PF_Attack )]  * 0.001f * sr;
        relSamp[p] = (float)a->v[playerParam( p, PF_Release )] * 0.001f * sr;

        // Note-on edge: reset the read head to StartPos and start the attack.
        if ( pl.trig )
        {
            pl.trig = false;
            pl.envState = Player::kAttack; pl.envCounter = 0.0f;
            if ( cfP[p] ) pl.slice.retrigger();
            else          pl.reader.start( (double)startPos01 * (double)kitbuffer::kNumSamples );
        }
        audible[p] = ( pl.envState != Player::kIdle );

        if ( cfP[p] )
        {
            // StartPos -> rank offset among slices (filter offset); Cutoff picks
            // the base nearest-centroid slice.
            float cutoff01 = (float)a->v[playerParam( p, PF_Cutoff )] / 1000.0f;
            pl.slice.selectPerBlock( a->engine->sliceAnalysis, cutoff01, startPos01,
                                     gHystSemis, sliceLenMs, gCrossfadeMs, sr );
        }
        else
        {
            // SliceLen (CF off) maps to the stretch "scale" (which octave period
            // to target): log-position of sliceLenMs across the 2..1024 ms set.
            float sc = ( logf( sliceLenMs ) - 0.6931472f ) / ( 6.9314718f - 0.6931472f );
            if ( sc < 0.0f ) sc = 0.0f;
            if ( sc > 1.0f ) sc = 1.0f;
            spP[p].scale01     = sc;
            spP[p].stretch     = stretchVal;
            spP[p].threshold01 = gThreshold01;
            spP[p].xfadeMs     = gXfadeMs;
        }
    }

    // Mix the 4 note-gated voices (each raw x its AHR envelope), normalize to
    // 8V, soft-clip to +-11V so the summed voices can't hard-clip the bus.
    for ( int i = 0; i < n; ++i )
    {
        float mix = 0.0f;
        for ( int p = 0; p < 4; ++p )
        {
            if ( !audible[p] ) continue;
            Player& pl = a->players[p];
            float raw = cfP[p] ? pl.slice.processSample( kb, advP[p] )
                               : pl.reader.process( kb, maps, spP[p], advP[p], 1.0f );
            mix += raw * pl.envProcess( atkSamp[p], relSamp[p] );
        }
        mix = filters::softClip( mix * normGain );
        if ( replace ) out[i] = mix; else out[i] += mix;
    }
}

static const _NT_factory factory = {
    .guid                         = NT_MULTICHAR( '4', 'x', '4', 'F' ),
    .name                         = "4x4 Matrix Feedback",
    .description                  = "Self-oscillating 4-processor feedback kit generator",
    .numSpecifications            = 0,
    .specifications               = NULL,
    .calculateStaticRequirements  = NULL,
    .initialise                   = NULL,
    .calculateRequirements        = calculateRequirements,
    .construct                    = construct,
    .parameterChanged             = NULL,   // all params read live in step() (see step())
    .step                         = step,
    .draw                         = NULL,
    .midiRealtime                 = NULL,
    .midiMessage                  = midiMessage,   // note on/off gates the 4 players
    .tags                         = kNT_tagInstrument,
    .hasCustomUi                  = NULL,
    .customUi                     = NULL,
    .setupUi                      = NULL,
    .serialise                    = NULL,
    .deserialise                  = NULL,
    .midiSysEx                    = NULL,
    .parameterUiPrefix            = NULL,
    .parameterString              = parameterString,   // Cutoff -> Hz readout
};

extern "C" uintptr_t pluginEntry( _NT_selector selector, uint32_t data )
{
    switch ( selector )
    {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return (uintptr_t)( data == 0 ? &factory : NULL );
    }
    return 0;
}
