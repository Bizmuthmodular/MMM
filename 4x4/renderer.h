// renderer.h — the throttled, chunked background computation that
// produces kit audio. Implementation Plan Step 14, per
// 4x4-System-Overview.md §6/§7.
//
// STATE CONTINUITY BETWEEN RENDERS — resolved here, per the plan's
// explicit instruction (§10 left this open): processor/filter/Reg
// internal state (P1's phase-vocoder buffers, P2's allpass history, P4's
// hold state, HPF/LPF histories, Reg's DC-block/AGC envelope, the
// feedback matrix's one-sample delay history) ALL RESET COLD at the start
// of every new render, via FeedbackNetwork::onNewRender(). Only Contour
// instances carry anything forward (their envelope LEVEL, via trigger()'s
// documented no-reset behavior, §6) — nothing else does. Reasoning: this
// project's entire design is built around seed-reproducibility (every
// random element is seed-derived, §2/§3/§6) — if a render's processor/Reg
// state depended on whatever happened to be left over from whatever
// render came before it, the same seed would stop producing the same
// kit, breaking that guarantee for no real benefit (unlike Contour's
// trigger(), which exists specifically to avoid a jarring level-reset
// transition — the processors' internal state doesn't have an equivalent
// "character worth preserving," it's just DSP plumbing). The starter wave
// (§1) already exists specifically to kick-start a cold network each
// render; carrying old processor state forward would undercut that by
// starting every render as a hidden continuation of the last one.
//
// CONTOUR SEEDING ACROSS RENDERS — UPDATED 2026-10-21. Originally the Contour
// kept one fixed seed for the plugin's lifetime (same modulation gesture every
// render, only the processor bases varied). That was reasonable when there
// were 21 separate engines (reseeding meant rebuilding all 21). Under the
// single-contour "playhead" design (§6) there is just ONE engine and its table
// is re-baked every render anyway, so per the user's request the Contour is now
// RESEEDED each render with the same per-render seed the processor bases use
// (baseSeed + renderIndex): both the curve shape and the playhead phase offsets
// are freshly randomised each render, giving a genuinely new modulation gesture
// per render (still governed by the settings bank). It stays fully
// deterministic/reproducible for a given render index, and costs nothing extra
// (the table was already re-baked every render — only the seed now varies).
//
// CHUNK SIZE: kChunkSize render samples are advanced per advanceChunk()
// call (intended to be called once per step()). Picked conservatively
// (16) since no on-device profiling exists yet (Step 16) to measure the
// actual peak-per-call cost of one internal render sample (30 Contour
// advances + the full 4-channel HPF/LPF/Pn/Reg chain + matrix mix).
// Deliberately a single named constant, easy to raise once real profiling
// data exists — smaller trades toward lower peak CPU and a slower bake,
// larger trades the other way, per §7 ("this trade is free to make
// because nothing downstream has a deadline").
//
// RENDER-RATE TO KIT-RATE CONVERSION: the feedback network runs at the
// render's own internal processing rate (NT_globals.sampleRate, e.g.
// 48kHz — checked at runtime, never hardcoded, per CLAUDE.md), but the
// kit buffer is fixed at 32kHz (§8). That ratio isn't generally an
// integer (48000/32000 = 1.5, but other sample rates give other,
// non-integer ratios), so a plain fixed-step decimator won't line up
// exactly. A fractional accumulator handles any ratio correctly: it
// advances by 1 every internal sample and emits a kit sample (holding the
// most recent internal sample's value) whenever it crosses the next
// kit-sample boundary. No anti-aliasing filter — consistent with this
// project's established stance that P3/P4's own aliasing is accepted
// character, not mitigated (§4/§5).
//
// 4-CHANNEL-TO-MONO MIXDOWN: not specified anywhere in the locked design
// (checked directly) — this step's own necessary decision, since the kit
// buffer is mono (§8) but the network has 4 independent "Out n" taps
// (§1). Sums all 4 channels (the standard way N independent signals
// combine onto one bus) with a documented 0.5x attenuation (6dB headroom)
// to keep ordinary correlated-content sums comfortably inside the kit
// buffer's own +-11V hard clamp, rather than relying on that clamp alone.
//
// No dynamic allocation, no STL, no exceptions/RTTI, C++11.
#pragma once
#include "contour_destinations.h"
#include "feedback_network.h"
#include "filters.h"
#include "kit_buffer.h"
#include "timestretch.h"
#include <cstdint>

namespace renderer {

// Internal render samples advanced per advanceChunk() (one call per step()).
// RENDER-BLEED NOTE (2026-10-21): the bake runs inside the real-time step()
// callback, so per-step render cost competes with the live audio for the CPU
// budget; when it (plus P1's periodic FFT) exceeded the module's 90% mute
// threshold the module muted outputs periodically — heard as periodic cracking
// in playback whenever a render ran. The primary fix was the single-contour
// "playhead" redesign (contour_destinations.h), which removed 20 of the 21
// per-sample Contour envelope generators — the dominant per-sample cost — so
// the per-step budget now has ample headroom even on an FFT step. kChunkSize
// is the remaining on-device tuning knob: if any cracking survives, LOWER it
// (smaller = lower peak per-step cost, proportionally longer bake — free per
// §7, the old kit keeps playing until the new one swaps in). Kept at 16 to
// preserve the ~6.6s (1x) first-bake time; lowering trades that away.
static const int kChunkSize = 16;
// Period-map hops analysed per step() during the post-render kAnalyzing phase
// (period-stretch.md). Bounded per-call work; the feedback render is already
// finished by then, so there is ample CPU headroom. ~2759 hops total, so a
// full analysis spreads over ~700 steps (~0.35s) after each bake.
static const int kAnalyzeHopsPerStep = 4;
static const float kMixdownGain = 0.5f;
// Loop-seam fade: the player loops the kit [0,N); the render starts cold (~0)
// but ends on a ringing tail (!=0), so without this the seam is a step
// discontinuity -> a click every loop. Fading the first/last kSeamFadeSamples
// to zero makes the seam 0->0 (seamless). ~8ms at 32kHz — short enough not to
// soften the onset meaningfully, applied once at render completion.
static const int kSeamFadeSamples = 256;

class Renderer
{
public:
    enum State { kIdle, kRendering, kAnalyzing };

    // Call once, e.g. from the plugin's construct() — analogous to the
    // one-time setup in CLAUDE.md's initialise()/construct() lifecycle.
    // Assigns Contour's seed for the plugin's entire lifetime (see file
    // header: never reseeded on an ordinary render trigger).
    void initialise( uint32_t baseSeed, float renderSampleRate )
    {
        baseSeed_ = baseSeed;
        renderIndex_ = 0;
        baseSampleRate_ = renderSampleRate;        // the module's rate (e.g. 48kHz)
        oversample_ = 1;
        pendingOversample_ = 1;
        renderSampleRate_ = baseSampleRate_;       // internal = base * oversample (1x here)
        contourdest::SharedSettings shared;        // defaults
        contourDest_.configure( shared, kitbuffer::kDurationSec, baseSeed, renderSampleRate_ );
        configured_ = shared;
        pending_ = shared;
        // Zero the period maps so playback before the first analysis completes
        // sees no periodicity (no spurious stretching on the initial silence).
        maps_[0].clear();
        maps_[1].clear();
        swapGen_ = 0;
        state_ = kIdle;
    }

    // --- Time-stretch period maps (per buffer, swapped with the kit) ---
    // The maps for the currently-playing kit. The player reads these; they are
    // only ever swapped in after their analysis fully completed (see
    // advanceChunk), so they are always valid for the current buffer.
    const timestretch::PeriodMaps& currentMaps( const kitbuffer::DoubleBufferedKit& kit ) const
    {
        return maps_[kit.activeIndex()];
    }
    // Increments on every kit swap; the player watches it to reset its read
    // head and rebind to the new buffer's maps (period-stretch.md).
    uint32_t swapGeneration() const { return swapGen_; }

    // Oversampling factor (1/2/4/8): the whole network runs at
    // oversample x baseSampleRate internally, then the mono mix is
    // anti-alias-filtered and decimated to the 32kHz kit (§7). Offline, so
    // the only cost is bake time (scales ~linearly) and NOT peak CPU per
    // step(). Stored as pending; applied at the next startNewRender (the
    // rate can't change mid-bake without corrupting the decimator). Clamped
    // to {1,2,4,8}.
    void setOversample( int factor )
    {
        int f = ( factor >= 8 ) ? 8 : ( factor >= 4 ) ? 4 : ( factor >= 2 ) ? 2 : 1;
        pendingOversample_ = f;
    }

    // Live-adjustable global Contour settings bank (new "Contour" parameter
    // page). Stored as pending; actually applied (reseed + rebuild of all 30
    // engines, which is why it can't happen mid-render) only at the next
    // startNewRender(), and only if it actually differs from what's already
    // configured — so an unchanged bank preserves Contour's carry-forward
    // between renders (§6), while a change takes effect on the next bake.
    // Seeding stays per-instance (baseSeed + destination index) throughout:
    // these are GLOBAL settings with INDIVIDUAL seeds, exactly as before.
    void setSharedSettings( const contourdest::SharedSettings& s ) { pending_ = s; }

    bool rendering() const { return state_ == kRendering; }
    uint32_t renderIndex() const { return renderIndex_; }

    // Performer-facing depth SCALES (0..1), per §2/§3: the real "P1/P2
    // depth" parameters don't replace the Contour-driven depth, they scale
    // how much of it reaches the processor. Final depth = scale * curve.
    // Both default to 1.0 (full curve) so an untouched plugin behaves as if
    // these were at 100%, since parameterChanged isn't called at construct.
    void setP1DepthScale( float s ) { p1DepthScale_ = s; }
    void setP2DepthScale( float s ) { p2DepthScale_ = s; }
    void setP3DepthScale( float s ) { p3DepthScale_ = s; }   // scales the delay-time modulation
    void setP4DepthScale( float s ) { p4DepthScale_ = s; }   // scales bits+divisor modulation

    // Static per-channel resonant bandpass (Filters page, 2026-10-20): center
    // frequency (Hz) and bandwidth (semitones, 1..96). Not Contour-modulated;
    // stored here and pushed into the network's biquads at each render start
    // (coeffs depend on the oversampled rate, so they're (re)computed there).
    void setBandpassFreq( int ch, float hz ) { bpFreq_[ch] = hz; }
    void setBandpassBw( int ch, float semitones ) { bpBwSemis_[ch] = semitones; }

    // VCFRND (Filters page): when on, each channel's bandpass CENTER FREQUENCY
    // is drawn fresh (log-uniform 20..16000 Hz, seed-derived from the per-render
    // seed so a given seed+render still reproduces) at every render, overriding
    // the static Freq params. When off, the Freq params are used. Read live;
    // takes effect at the next render (resolved in startNewRender).
    void setVcfRnd( bool on ) { vcfRnd_ = on; }
    // QCONT (Filters page): when on, the bandpass BANDWIDTH ("Q") of all 4
    // channels is driven by one shared Contour curve (recomputing biquad
    // coeffs per internal sample), overriding the static BW params. When off,
    // the BW params are used (coeffs set once per render). Latched per render.
    void setQCont( bool on ) { qCont_ = on; }

    // Normalize level (Render page, 0..11V, 2026-10-21): the baked kit is
    // peak-normalized to this level at PLAYBACK — playbackGain() = level /
    // currentKitPeak. Read live, so it acts as a live output-level knob; since
    // the player uses linear interpolation (which never overshoots its sample
    // endpoints) the output peak can't exceed `level`, so any level <= 11V
    // guarantees clean headroom — the clipping fix the user asked for.
    void setNormalizeLevel( float v ) { normalizeLevel_ = v; }
    // Gain the player multiplies each kit sample by. 0 until the first kit has
    // baked (currentPeak_ == 0 -> silent, no divide-by-zero).
    float playbackGain() const { return currentPeak_ > 1e-4f ? normalizeLevel_ / currentPeak_ : 0.0f; }
    float currentKitPeak() const { return currentPeak_; }   // for tests/UI

    // Starter-wave RANDOMIZATION ranges (2026-10-24). Each render, every
    // channel's starter gets a fresh random frequency in [minF,maxF], length in
    // [minLenSec,maxLenSec], and waveform (square/saw/triangle/noise), all
    // seed-derived at startNewRender. Set live from the Starter page.
    void setStarterRanges( float minF, float maxF, float minLenSec, float maxLenSec )
    {
        starterMinFreq_ = minF; starterMaxFreq_ = maxF;
        starterMinLen_ = minLenSec; starterMaxLen_ = maxLenSec;
    }

    // Per-processor on/off (channel 0..3 = P1..P4). Applied live into each
    // render sample's ChannelControls — a disabled channel is muted in and
    // out, so it affects the render currently baking and every render after,
    // but not an already-baked kit the player is looping (re-bake to hear it).
    void setProcessorEnabled( int ch, bool on ) { procEnabled_[ch] = on; }

    // P1 spectral manipulation (Shifter page, 2026-10-22). BlockSize is a bin
    // count {1,2,4,8,16,32,64}; Swaps/SwapSize/Omit are 0..100% of the current
    // BlockSize's maxima. Stored here and latched into P1's maps at the next
    // render start (so they can't change mid-render — phase coherence needs a
    // fixed map). Defaults = no swaps / no omit = clean full-spectrum shift.
    void setP1BlockSize( int bins )    { p1BlockSize_ = bins; }
    void setP1Swaps( float pct )       { p1SwapsPct_ = pct; }
    void setP1SwapSize( float pct )    { p1SwapSizePct_ = pct; }
    void setP1Omit( float pct )        { p1OmitPct_ = pct; }

    // Trigger a new render. Re-triggers Contour (same seed, carries level
    // forward, §6) and cold-resets the feedback network's processor/
    // filter/Reg state (see file header). P1/P2 get a genuinely fresh
    // random base this render via the incrementing render index folded
    // into the seed passed to FeedbackNetwork::onNewRender().
    void startNewRender()
    {
        // Lock in the oversampling factor for this bake and set the internal
        // rate accordingly. Done before the Contour reconfigure below so the
        // engines are (re)built at the correct rate.
        oversample_ = pendingOversample_;
        renderSampleRate_ = baseSampleRate_ * (float)oversample_;
        aaFilter_.reset();

        // Apply a changed Contour settings bank first (reseed + rebuild all
        // 30 engines). Skipped when unchanged AND the rate is unchanged, so
        // carry-forward is preserved in the common case; a rate change forces
        // a reconfigure since the engines' setSampleRate must be refreshed.
        if ( !settingsEqual( pending_, configured_ ) || renderSampleRate_ != configuredRate_ )
        {
            contourDest_.configure( pending_, kitbuffer::kDurationSec, baseSeed_, renderSampleRate_ );
            configured_ = pending_;
            configuredRate_ = renderSampleRate_;
        }
        uint32_t perRenderSeed = baseSeed_ + renderIndex_;
        // Re-seed the Contour with this render's seed so its curve + playheads
        // are freshly randomised each render (2026-10-21), the same way the
        // processor bases are — reproducible for a given render index.
        contourDest_.onNewRender( perRenderSeed );
        // Latch P1's spectral-manipulation params into its maps for this bake
        // (must be set before onNewRender, which regenerates the maps).
        net_.setP1Spectral( p1BlockSize_, p1SwapsPct_, p1SwapSizePct_, p1OmitPct_ );
        net_.onNewRender( perRenderSeed );
        // Randomize each channel's starter (freq/length/waveform) for this bake.
        for ( int ch = 0; ch < 4; ++ch )
        {
            uint32_t h = perRenderSeed * 2246822519u + (uint32_t)ch * 3266489917u + 0x27D4EB2Fu;
            h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
            float rF = (float)h / 4294967296.0f;
            float rL = (float)( ( h >> 11 ) ^ ( h << 7 ) ) / 4294967296.0f; if ( rL < 0 ) rL = -rL; if ( rL > 1 ) rL -= (int)rL;
            feedbacknet::StarterWave& sw = net_.starter( ch );
            float lo = starterMinFreq_, hi = starterMaxFreq_; if ( hi < lo ) { float t = lo; lo = hi; hi = t; }
            sw.freqHz = lo + rF * ( hi - lo );
            float llo = starterMinLen_, lhi = starterMaxLen_; if ( lhi < llo ) { float t = llo; llo = lhi; lhi = t; }
            sw.durationSec = llo + rL * ( lhi - llo );
            sw.waveform = (int)( h >> 29 ) & 3;   // 0..3: square/saw/triangle/noise
        }
        // Resolve this render's bandpass center frequencies: VCFRND draws a
        // fresh seed-derived cutoff per channel, otherwise the static Freq
        // params are used. Latch QCONT for the whole bake (toggling it mid-bake
        // mustn't change the render in progress — consistent with oversample).
        activeQCont_ = qCont_;
        for ( int c = 0; c < 4; ++c )
            activeFreq_[c] = vcfRnd_ ? randomCutoff( perRenderSeed, c ) : bpFreq_[c];
        // (Re)compute the 4 bandpass biquads at the now-known internal rate
        // (onNewRender() cleared their state above). When QCONT is on these are
        // overwritten per internal sample in advanceChunk; this is the initial
        // state so sample 0 isn't a bare passthrough.
        for ( int c = 0; c < 4; ++c )
            net_.setBandpassParams( c, activeFreq_[c], bpBwSemis_[c], renderSampleRate_ );
        ++renderIndex_;
        kitSampleIndex_ = 0;
        decimAccumulator_ = 0.0f;
        bakingPeak_ = 0.0f;        // running peak of this bake, for normalization
        state_ = kRendering;
    }

    // Advances up to kChunkSize internal render samples. No-op if not
    // currently rendering. Returns true exactly on the call that finishes
    // the render (the kit buffer has already been swapped by then).
    bool advanceChunk( kitbuffer::DoubleBufferedKit& kit )
    {
        // Post-render analysis phase: the bake is finished and finalized; build
        // the just-baked buffer's period maps a bounded number of hops per call,
        // THEN swap. Runs with the feedback render idle, so it has CPU headroom
        // and never competes with the live audio (protects the render-bleed fix).
        if ( state_ == kAnalyzing )
        {
            if ( analyzer_.advance( kAnalyzeHopsPerStep ) )
            {
                // Assign the normalization peak AT the swap (not at render
                // completion) so currentPeak_ always matches the buffer the
                // player is actually reading — otherwise, during the ~0.35s
                // analysis window, the still-playing OLD kit would be scaled by
                // the NEW kit's peak (an audible level jump).
                currentPeak_ = bakedPeak_;
                kit.swap();           // atomically make the baked buffer + its maps current
                ++swapGen_;           // tell the player to reset its read head
                state_ = kIdle;
                return true;          // this call finished the render (maps ready, swapped)
            }
            return false;
        }

        if ( state_ != kRendering ) return false;

        float samplesPerKitSample = renderSampleRate_ / (float)kitbuffer::kSampleRateHz;
        for ( int i = 0; i < kChunkSize && kitSampleIndex_ < kitbuffer::kNumSamples; ++i )
        {
            contourDest_.advance();
            // QCONT: drive all 4 bandpass bandwidths from the shared Q Contour
            // curve this sample, recomputing coeffs. Center freq stays fixed at
            // this render's resolved value (static or VCFRND-random). Only when
            // on — the static path leaves the render-start coeffs untouched.
            if ( activeQCont_ )
            {
                float qbw = contourDest_.qBandwidthSemis();
                for ( int c = 0; c < 4; ++c )
                    net_.setBandpassParams( c, activeFreq_[c], qbw, renderSampleRate_ );
            }
            feedbacknet::ChannelControls ctrl;
            contourDest_.fillChannelControls( ctrl );   // Contour offsets (raw)
            contourDest_.fillMatrix( net_ );
            // Apply performer depth scales on top of the Contour-driven
            // modulation (§2/§3): the parameter scales the curve offset, the
            // processor adds its own randomized base. Same model for all 4.
            ctrl.p1DepthSemitones *= p1DepthScale_;
            ctrl.p2DepthHz        *= p2DepthScale_;
            ctrl.p3DelayMs        *= p3DepthScale_;
            ctrl.p4FreqOffset     *= p4DepthScale_;   // P4 phaser center-freq sweep
            // Bandpass filters are already configured (render start); only the
            // enable flags need per-sample passing here.
            for ( int c = 0; c < 4; ++c )
                ctrl.processorEnabled[c] = procEnabled_[c];

            float out[4];
            net_.processSample( renderSampleRate_, ctrl, out );
            float mixed = ( out[0] + out[1] + out[2] + out[3] ) * kMixdownGain;

            // Anti-clip: each channel is tanh-softclipped to +-11V individually,
            // but 0.5*(4 channels) can still reach +-22V, which the kit's 12-bit
            // writer would HARD-clamp at +-11V — brick-wall clipping heard as
            // pops. Soft-clip the mix (same tanh, 11V ceiling) BEFORE the AA
            // filter so the clip's harmonics get anti-aliased too and the AA
            // output (gain <= 1) stays safely within the +-11V kit range — no
            // hard-clamp events, so no clipping pops. (2026-10-21)
            float clipped = filters::softClip( mixed );

            // Anti-alias before decimating to the kit rate: the filter runs
            // every internal (oversampled) sample and the decimator samples
            // its output. This removes the content that would otherwise fold
            // into the 32kHz kit — the mechanism that makes oversampling
            // actually reduce in-band aliasing (§7). Runs at every factor,
            // including 1x (where it cleans the plain 48->32 fold).
            float aa = aaFilter_.process( clipped, renderSampleRate_ );

            decimAccumulator_ += 1.0f;
            if ( decimAccumulator_ >= samplesPerKitSample )
            {
                decimAccumulator_ -= samplesPerKitSample;
                kit.inactive().write( kitSampleIndex_, aa );
                // Track the peak over the NON-faded body only; the faded edges'
                // max is folded in at completion (applyLoopSeamFades returns it).
                // Tracking the whole buffer pre-fade would overestimate whenever
                // the global peak sits in the starter region (first
                // kSeamFadeSamples, which then get faded down), making
                // normalization undershoot the set level.
                if ( kitSampleIndex_ >= kSeamFadeSamples &&
                     kitSampleIndex_ < kitbuffer::kNumSamples - kSeamFadeSamples )
                {
                    float pk = aa < 0.0f ? -aa : aa;
                    if ( pk > bakingPeak_ ) bakingPeak_ = pk;
                }
                ++kitSampleIndex_;
            }
        }

        if ( kitSampleIndex_ >= kitbuffer::kNumSamples )
        {
            float fadeMax = applyLoopSeamFades( kit.inactive() );   // seamless loop: fade edges to 0
            // Post-fade stored peak = max(body peak, faded-edge peak). Held in
            // bakedPeak_ and only promoted to currentPeak_ at the swap below, so
            // the playing buffer and its normalization peak always agree.
            bakedPeak_ = bakingPeak_ > fadeMax ? bakingPeak_ : fadeMax;
            // Do NOT swap yet: first build this (now-finalized) buffer's period
            // maps in the kAnalyzing phase, into the maps slot that matches the
            // inactive kit buffer, so the swap brings audio + maps in together.
            analyzer_.begin( &kit.inactive(), &maps_[1 - kit.activeIndex()] );
            state_ = kAnalyzing;
            return false;
        }
        return false;
    }

private:
    // Fade the first and last kSeamFadeSamples of the freshly-baked kit to
    // zero so the player's loop seam (kit[N-1] -> kit[0]) is 0 -> 0 and clicks
    // vanish. Read-modify-write on the 12-bit store; one-time, ~2*256 ops.
    // Fades the first/last kSeamFadeSamples to zero (seamless loop) and RETURNS
    // the max |faded sample| so the caller can compute the true post-fade peak.
    static float applyLoopSeamFades( kitbuffer::KitBuffer& kb )
    {
        const int N = kitbuffer::kNumSamples;
        int F = kSeamFadeSamples;
        if ( N < 2 * F ) F = N / 2;
        float mx = 0.0f;
        if ( F < 1 ) return mx;
        for ( int i = 0; i < F; ++i )
        {
            float w = (float)i / (float)F;        // 0 at the very edge -> ~1 inward
            float a = kb.read( i ) * w; kb.write( i, a );        // fade-in:  kit[0]   -> 0
            float aa = a < 0.0f ? -a : a; if ( aa > mx ) mx = aa;
            int j = N - 1 - i;
            float b = kb.read( j ) * w; kb.write( j, b );        // fade-out: kit[N-1] -> 0
            float bb = b < 0.0f ? -b : b; if ( bb > mx ) mx = bb;
        }
        return mx;
    }

    static bool settingsEqual( const contourdest::SharedSettings& a, const contourdest::SharedSettings& b )
    {
        return a.windows == b.windows
            && a.chaos == b.chaos
            && a.radiusSec == b.radiusSec
            && a.roundProb == b.roundProb
            && a.holdProb == b.holdProb;
    }

    // Log-uniform random cutoff in [20, 16000] Hz (the Freq param range) from a
    // seed+channel hash (mulberry32-style finalizer). Deterministic: the same
    // seed+ch always yields the same cutoff, so a given render stays
    // reproducible — consistent with every other random element in this design.
    static float randomCutoff( uint32_t seed, int ch )
    {
        uint32_t s = seed * 2654435761u + (uint32_t)ch * 40503u + 0x9E3779B9u;
        s ^= s >> 16; s *= 0x7feb352du; s ^= s >> 15; s *= 0x846ca68bu; s ^= s >> 16;
        float r = (float)s / 4294967296.0f;              // [0,1)
        return 20.0f * powf( 16000.0f / 20.0f, r );       // log-uniform 20..16000
    }

    contourdest::ContourDestinations contourDest_;
    feedbacknet::FeedbackNetwork net_;
    filters::AntiAliasLowpass aaFilter_;        // pre-decimation anti-aliasing (§7)
    timestretch::PeriodMaps maps_[2];           // per-buffer period maps (swap with the kit)
    timestretch::Analyzer analyzer_;            // builds the inactive buffer's maps post-render
    uint32_t swapGen_ = 0;                       // ++ on each swap; player watches to reset its read head
    contourdest::SharedSettings configured_;   // what the engines are currently built with
    contourdest::SharedSettings pending_;       // what the params currently ask for
    State state_ = kIdle;
    uint32_t baseSeed_ = 0;
    uint32_t renderIndex_ = 0;
    float baseSampleRate_ = 48000.0f;           // the module rate (NT_globals.sampleRate)
    float renderSampleRate_ = 48000.0f;         // internal = base * oversample
    float configuredRate_ = 0.0f;               // rate the Contour engines were built at
    int oversample_ = 1;                        // active factor this bake
    int pendingOversample_ = 1;                 // factor to apply at the next startNewRender
    int kitSampleIndex_ = 0;
    float decimAccumulator_ = 0.0f;
    float p1DepthScale_ = 1.0f;
    float p2DepthScale_ = 1.0f;
    float p3DepthScale_ = 1.0f;
    float p4DepthScale_ = 1.0f;
    // Static per-channel resonant bandpass (Filters page): staggered default
    // centers so the 4 channels occupy distinct bands out of the box; 4-octave
    // (48 semitone) default width.
    float starterMinFreq_ = 50.0f, starterMaxFreq_ = 500.0f;    // Hz (Starter page)
    float starterMinLen_ = 0.1f, starterMaxLen_ = 1.0f;         // seconds
    float bpFreq_[4] = { 200.0f, 600.0f, 1500.0f, 4000.0f };
    float bpBwSemis_[4] = { 48.0f, 48.0f, 48.0f, 48.0f };
    bool procEnabled_[4] = { true, true, true, true };
    // P1 spectral manipulation (Shifter page). Defaults = clean full-spectrum
    // shift (no swaps, no omit), B=8.
    int   p1BlockSize_ = 8;
    float p1SwapsPct_ = 0.0f;
    float p1SwapSizePct_ = 0.0f;
    float p1OmitPct_ = 0.0f;
    // VCFRND / QCONT (Filters page). Default off: an untouched plugin uses the
    // static Freq/BW params exactly as before. vcfRnd_/qCont_ are read live;
    // activeFreq_/activeQCont_ are latched per render in startNewRender.
    bool vcfRnd_ = false;
    bool qCont_ = false;
    bool activeQCont_ = false;
    float activeFreq_[4] = { 200.0f, 600.0f, 1500.0f, 4000.0f };
    // Playback peak-normalization (Render page). normalizeLevel_ default 8V
    // matches the param default; currentPeak_ is the playing kit's true peak,
    // bakingPeak_ accumulates during the in-progress bake.
    float normalizeLevel_ = 8.0f;
    float currentPeak_ = 0.0f;   // peak of the PLAYING kit (matches current())
    float bakedPeak_ = 0.0f;     // peak of the just-baked kit, promoted at swap
    float bakingPeak_ = 0.0f;    // running peak during the in-progress bake
};

} // namespace renderer
