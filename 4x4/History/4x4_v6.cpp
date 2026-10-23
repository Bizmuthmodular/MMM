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

// Everything large, placed into the DRAM block the firmware provides.
struct Engine
{
    renderer::Renderer         renderer;
    kitbuffer::DoubleBufferedKit kit;
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

struct FourByFourAlg : public _NT_algorithm
{
    Engine* engine;
    double  playPos;    // fractional kit read position for 32kHz->sampleRate playback
    int     prevRegen;  // previous Regenerate value, for 0->1 edge detection in step()

    // Initialize only our OWN members. Deliberately do NOT touch the base's
    // v / vIncludingCommon — those are system-managed (api.h), and the
    // monosynth example likewise leaves them alone so a firmware-set pointer
    // is never clobbered. parameters / parameterPages are set in construct().
    FourByFourAlg() : engine( nullptr ), playPos( 0.0 ), prevRegen( 0 ) {}
};

// Fixed base seed for reproducibility (§10's seed-derived-vs-entropy
// question stays open; a fixed constant is the reproducible choice, and the
// renderIndex increments per render so successive bakes still differ).
static const uint32_t kBaseSeed = 0x34783446u;   // "4x4F"

enum {
    kParamStarterFreq1, kParamStarterFreq2, kParamStarterFreq3, kParamStarterFreq4,
    kParamStarterLen1,  kParamStarterLen2,  kParamStarterLen3,  kParamStarterLen4,
    kParamP1Depth,
    kParamP2Depth,
    kParamP3Depth,      // delay-time modulation depth (P1/P2 model)
    kParamP4Depth,      // phaser center-freq sweep depth (P1/P2 model)
    kParamRegenerate,
    kParamPlay,
    kParamOversample,   // render-quality: 1x/2x/4x/8x internal oversampling
    kParamWindows,      // Contour settings bank (global, per-instance seeds)
    kParamChaos,
    kParamRadius,
    kParamRoundProb,
    kParamHoldProb,
    kParamBpFreq1, kParamBpFreq2, kParamBpFreq3, kParamBpFreq4,   // per-channel resonant bandpass: center freq
    kParamBpBw1, kParamBpBw2, kParamBpBw3, kParamBpBw4,          // ...and bandwidth (semitones)
    kParamP1Enable,     // per-processor on/off (muted in & out when off)
    kParamP2Enable,
    kParamP3Enable,
    kParamP4Enable,
    kParamOutput,
    kParamOutputMode,
    // NEW PARAMS GO HERE, AT THE END. Never insert mid-list: the firmware
    // restores a saved preset's values BY INDEX, so shifting an existing
    // param's index remaps stale values onto it (that's what sent Output ->
    // Input 1 when Pitch was briefly inserted mid-list, 2026-10-21).
    kParamPitch,        // playback pitch via speed, +-48 semitones (+-4 octaves)
    kParamVcfRnd,       // Filters: randomize bandpass center freqs every render
    kParamQCont,        // Filters: modulate bandpass Q (bandwidth) with a Contour
    kParamNormLevel,    // Render: peak level (V) the kit is normalized to at playback
};

static char const* const regenStrings[] = { "Hold", "Regenerate", NULL };
static char const* const playStrings[]  = { "Off", "On", NULL };
static char const* const osStrings[]    = { "1x", "2x", "4x", "8x", NULL };

static const _NT_parameter parameters[] = {
    { .name = "S1 freq", .min = 20, .max = 2000, .def = 110, .unit = kNT_unitHz, .scaling = 0, .enumStrings = NULL },   // 0
    { .name = "S2 freq", .min = 20, .max = 2000, .def = 110, .unit = kNT_unitHz, .scaling = 0, .enumStrings = NULL },   // 1
    { .name = "S3 freq", .min = 20, .max = 2000, .def = 110, .unit = kNT_unitHz, .scaling = 0, .enumStrings = NULL },   // 2
    { .name = "S4 freq", .min = 20, .max = 2000, .def = 110, .unit = kNT_unitHz, .scaling = 0, .enumStrings = NULL },   // 3
    { .name = "S1 len", .min = 1, .max = 500, .def = 200, .unit = kNT_unitMs, .scaling = 0, .enumStrings = NULL },      // 4
    { .name = "S2 len", .min = 1, .max = 500, .def = 200, .unit = kNT_unitMs, .scaling = 0, .enumStrings = NULL },      // 5
    { .name = "S3 len", .min = 1, .max = 500, .def = 200, .unit = kNT_unitMs, .scaling = 0, .enumStrings = NULL },      // 6
    { .name = "S4 len", .min = 1, .max = 500, .def = 200, .unit = kNT_unitMs, .scaling = 0, .enumStrings = NULL },      // 7
    { .name = "P1 depth", .min = 0, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL }, // 8
    { .name = "P2 depth", .min = 0, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL }, // 9
    { .name = "P3 depth", .min = 0, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL }, // 10
    { .name = "P4 depth", .min = 0, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL }, // 11
    { .name = "Regenerate", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = regenStrings }, // 12
    { .name = "Play", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = playStrings }, // 13
    { .name = "Oversample", .min = 0, .max = 3, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = osStrings }, // 14
    { .name = "Windows", .min = 1, .max = 32, .def = 6, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL },        // 15
    { .name = "Chaos", .min = 0, .max = 100, .def = 60, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },     // 16
    { .name = "Radius", .min = 0, .max = 500, .def = 120, .unit = kNT_unitMs, .scaling = 0, .enumStrings = NULL },        // 17
    { .name = "Round prob", .min = 0, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL }, // 18
    { .name = "Hold prob", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },   // 19
    { .name = "Freq 1", .min = 20, .max = 16000, .def = 200, .unit = kNT_unitHz, .scaling = 0, .enumStrings = NULL },     // 20  bandpass centers,
    { .name = "Freq 2", .min = 20, .max = 16000, .def = 600, .unit = kNT_unitHz, .scaling = 0, .enumStrings = NULL },     // 21  staggered so the
    { .name = "Freq 3", .min = 20, .max = 16000, .def = 1500, .unit = kNT_unitHz, .scaling = 0, .enumStrings = NULL },    // 22  4 channels occupy
    { .name = "Freq 4", .min = 20, .max = 16000, .def = 4000, .unit = kNT_unitHz, .scaling = 0, .enumStrings = NULL },    // 23  distinct bands
    { .name = "BW 1", .min = 1, .max = 96, .def = 48, .unit = kNT_unitSemitones, .scaling = 0, .enumStrings = NULL },     // 24  bandwidth, 1 semitone
    { .name = "BW 2", .min = 1, .max = 96, .def = 48, .unit = kNT_unitSemitones, .scaling = 0, .enumStrings = NULL },     // 25  .. 8 octaves (96 st)
    { .name = "BW 3", .min = 1, .max = 96, .def = 48, .unit = kNT_unitSemitones, .scaling = 0, .enumStrings = NULL },     // 26
    { .name = "BW 4", .min = 1, .max = 96, .def = 48, .unit = kNT_unitSemitones, .scaling = 0, .enumStrings = NULL },     // 27
    { .name = "P1 pitch", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = playStrings },  // 28
    { .name = "P2 freq",  .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = playStrings },  // 29
    { .name = "P3 delay", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = playStrings },  // 30
    { .name = "P4 phaser", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = playStrings }, // 31
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output", 1, 13 )   // 32, 33
    // Pitch appended LAST (index 34) so adding it never shifts Output/other
    // indices — see the enum note. Playback speed, +-48 semitones (+-4 oct).
    { .name = "Pitch", .min = -48, .max = 48, .def = 0, .unit = kNT_unitSemitones, .scaling = 0, .enumStrings = NULL },   // 34
    // VCFRND / QCONT appended LAST (indices 35, 36), same end-of-list rule.
    // Both default Off, so a preset from the pre-feature version loads with the
    // static Freq/BW behavior unchanged.
    { .name = "VCFRND", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = playStrings },  // 35
    { .name = "QCONT",  .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = playStrings },  // 36
    // Render output level: the baked kit is peak-normalized to this at playback
    // (0..11V). Appended LAST (index 37) per the end-of-list ordering rule.
    { .name = "Norm level", .min = 0, .max = 11, .def = 8, .unit = kNT_unitVolts, .scaling = 0, .enumStrings = NULL }, // 37
};

static const uint8_t pageStarter[] = { kParamStarterFreq1, kParamStarterFreq2, kParamStarterFreq3, kParamStarterFreq4,
                                       kParamStarterLen1, kParamStarterLen2, kParamStarterLen3, kParamStarterLen4 };
static const uint8_t pageDepth[]   = { kParamP1Depth, kParamP2Depth, kParamP3Depth, kParamP4Depth };
static const uint8_t pageRender[]  = { kParamRegenerate, kParamPlay, kParamOversample, kParamPitch, kParamNormLevel };
static const uint8_t pageContour[] = { kParamWindows, kParamChaos, kParamRadius, kParamRoundProb, kParamHoldProb };
static const uint8_t pageFilters[] = { kParamBpFreq1, kParamBpFreq2, kParamBpFreq3, kParamBpFreq4,
                                       kParamBpBw1, kParamBpBw2, kParamBpBw3, kParamBpBw4,
                                       kParamVcfRnd, kParamQCont };
static const uint8_t pageProcessors[] = { kParamP1Enable, kParamP2Enable, kParamP3Enable, kParamP4Enable };
static const uint8_t pageRouting[] = { kParamOutput, kParamOutputMode };

static const _NT_parameterPage pages[] = {
    { .name = "Starter",    .numParams = ARRAY_SIZE( pageStarter ),    .group = 0, .params = pageStarter },
    { .name = "Depth",      .numParams = ARRAY_SIZE( pageDepth ),      .group = 0, .params = pageDepth },
    { .name = "Render",     .numParams = ARRAY_SIZE( pageRender ),     .group = 0, .params = pageRender },
    { .name = "Contour",    .numParams = ARRAY_SIZE( pageContour ),    .group = 0, .params = pageContour },
    { .name = "Filters",    .numParams = ARRAY_SIZE( pageFilters ),    .group = 0, .params = pageFilters },
    { .name = "Processors", .numParams = ARRAY_SIZE( pageProcessors ), .group = 0, .params = pageProcessors },
    { .name = "Routing",    .numParams = ARRAY_SIZE( pageRouting ),    .group = 0, .params = pageRouting },
};

static const _NT_parameterPages parameterPages = {
    .numPages = ARRAY_SIZE( pages ),
    .pages    = pages,
};

// Push all continuously-applied parameter values from v[] into the renderer.
// Used at construct (v[] holds defaults there, per CLAUDE.md) and mirrored
// per-parameter in parameterChanged.
static void applyAllParams( FourByFourAlg* a )
{
    renderer::Renderer& r = a->engine->renderer;
    for ( int i = 0; i < 4; ++i )
    {
        r.setStarterFreq( i, (float)a->v[kParamStarterFreq1 + i] );
        r.setStarterDuration( i, (float)a->v[kParamStarterLen1 + i] * 0.001f );   // ms -> s
    }
    r.setP1DepthScale( (float)a->v[kParamP1Depth] / 100.0f );
    r.setP2DepthScale( (float)a->v[kParamP2Depth] / 100.0f );
    r.setP3DepthScale( (float)a->v[kParamP3Depth] / 100.0f );
    r.setP4DepthScale( (float)a->v[kParamP4Depth] / 100.0f );

    // Static per-channel resonant bandpass (Filters page) — NOT Contour-
    // modulated; center freq (Hz) + bandwidth (semitones). Applied to the
    // biquads at the next render start (renderer.h).
    for ( int i = 0; i < 4; ++i )
    {
        r.setBandpassFreq( i, (float)a->v[kParamBpFreq1 + i] );
        r.setBandpassBw( i, (float)a->v[kParamBpBw1 + i] );
    }
    // VCFRND: randomize bandpass center freqs every render (overrides Freq).
    // QCONT: Contour-modulate bandpass Q/bandwidth (overrides static BW).
    r.setVcfRnd( a->v[kParamVcfRnd] != 0 );
    r.setQCont( a->v[kParamQCont] != 0 );

    // Render output level: peak-normalize the kit to this many volts at playback.
    r.setNormalizeLevel( (float)a->v[kParamNormLevel] );

    // Global Contour settings bank (new "Contour" page). Stored as pending;
    // the renderer applies them (reseed + rebuild) only at the next render
    // start, and only if changed (see renderer.h). Per-instance seeding is
    // unchanged — these are global settings with individual seeds.
    contourdest::SharedSettings cs;
    cs.windows   = (int)a->v[kParamWindows];
    cs.chaos     = (float)a->v[kParamChaos] / 100.0f;
    cs.radiusSec = (float)a->v[kParamRadius] / 1000.0f;   // ms -> s
    cs.roundProb = (float)a->v[kParamRoundProb] / 100.0f;
    cs.holdProb  = (float)a->v[kParamHoldProb] / 100.0f;
    r.setSharedSettings( cs );

    // Per-processor on/off (new "Processors" page). Muted channels add
    // nothing to the mix or feedback; takes effect on the render currently
    // baking and all later ones (re-bake to hear a change — an already-baked
    // kit is fixed audio).
    r.setProcessorEnabled( 0, a->v[kParamP1Enable] != 0 );
    r.setProcessorEnabled( 1, a->v[kParamP2Enable] != 0 );
    r.setProcessorEnabled( 2, a->v[kParamP3Enable] != 0 );
    r.setProcessorEnabled( 3, a->v[kParamP4Enable] != 0 );

    // Oversampling factor (new on the Render page). Enum index 0..3 -> 1/2/4/8x
    // (1 << index). Applied at the next render start (renderer.h).
    r.setOversample( 1 << a->v[kParamOversample] );
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

    // Regenerate: a momentary trigger, edge-detected (0->1) on the live v[]
    // value here in step() rather than in parameterChanged — the earlier
    // parameterChanged-based version did not reliably fire on the module.
    // On the rising edge, start a fresh bake and reset the parameter to Hold
    // so it behaves as a momentary button (NT_setParameterFromAudio is the
    // intended way to write a parameter from the audio context).
    int regen = a->v[kParamRegenerate];
    if ( regen == 1 && a->prevRegen == 0 )
    {
        a->engine->renderer.startNewRender();
        int32_t idx = NT_algorithmIndex( self );
        NT_setParameterFromAudio( idx, kParamRegenerate + NT_parameterOffset(), 0 );
    }
    // Re-read so the edge re-arms: once the auto-reset to Hold lands (this
    // step if the write is synchronous, a later step if deferred), prevRegen
    // becomes 0 and the next Hold->Regenerate counts as a fresh trigger.
    // While the value is held at Regenerate, prevRegen stays 1 and we don't
    // spam renders.
    a->prevRegen = a->v[kParamRegenerate];

    // 1. Background render: advance one bounded chunk per step() call (§7).
    a->engine->renderer.advanceChunk( a->engine->kit );

    // 2. Player (interim Play/loop, §6).
    int n = numFramesBy4 * 4;
    int outBus = a->v[kParamOutput];
    if ( outBus < 1 ) return;
    float* out = busFrames + ( outBus - 1 ) * n;
    bool replace = a->v[kParamOutputMode];
    bool play    = a->v[kParamPlay];

    if ( !play )
    {
        if ( replace )
            for ( int i = 0; i < n; ++i ) out[i] = 0.0f;   // Play=Off -> silence
        return;
    }

    // Pitch via playback speed (Render page, +-48 semitones = +-4 octaves):
    // ratio = 2^(semitones/12). Higher pitch reads the kit faster (shorter,
    // higher); lower pitch reads it slower (longer, lower) — exactly as asked.
    // Read live, so it takes effect immediately (pure playback, no re-bake).
    float pitchSemis = (float)a->v[kParamPitch];
    double pitchRatio = pow( 2.0, (double)pitchSemis / 12.0 );

    const kitbuffer::KitBuffer& kb = a->engine->kit.current();
    // Peak-normalize to the Norm level param (live). Linear interpolation never
    // overshoots its endpoints, so the output peak <= normGain*kitPeak = level,
    // guaranteeing no clipping for any level <= 11V.
    float normGain = a->engine->renderer.playbackGain();
    const double stepPerFrame = ( (double)kitbuffer::kSampleRateHz / (double)NT_globals.sampleRate ) * pitchRatio;
    double pos = a->playPos;
    for ( int i = 0; i < n; ++i )
    {
        int i0 = (int)pos;
        int i1 = i0 + 1;
        if ( i1 >= kitbuffer::kNumSamples ) i1 = 0;
        float frac = (float)( pos - (double)i0 );
        float s = ( kb.read( i0 ) * ( 1.0f - frac ) + kb.read( i1 ) * frac ) * normGain;
        if ( replace ) out[i] = s; else out[i] += s;
        pos += stepPerFrame;
        while ( pos >= (double)kitbuffer::kNumSamples ) pos -= (double)kitbuffer::kNumSamples;
    }
    a->playPos = pos;
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
    .midiMessage                  = NULL,
    .tags                         = kNT_tagInstrument,
    .hasCustomUi                  = NULL,
    .customUi                     = NULL,
    .setupUi                      = NULL,
    .serialise                    = NULL,
    .deserialise                  = NULL,
    .midiSysEx                    = NULL,
    .parameterUiPrefix            = NULL,
    .parameterString              = NULL,
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
