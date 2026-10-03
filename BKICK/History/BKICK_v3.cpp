#include "distingnt/api.h"
#include <cmath>
#include <new>

// ----------------------------------------------------------------------------
// Curve shaping
// Controls how much exponential/logarithmic curves deviate from linear.
// Increase for more dramatic curves, decrease for subtler shaping.
// ----------------------------------------------------------------------------

static const float kCurveBase = 3.0f;

// ----------------------------------------------------------------------------
// Envelope states
// ----------------------------------------------------------------------------

enum EnvState : uint8_t {
    kEnvIdle    = 0,
    kEnvAttack  = 1,
    kEnvHold    = 2,
    kEnvRelease = 3,
};

// ----------------------------------------------------------------------------
// Algorithm struct
// ----------------------------------------------------------------------------

struct BKICK : public _NT_algorithm
{
    // Cached derived values (updated in parameterChanged)
    float freqInc;          // VCO phase increment per sample
    float levelV;           // peak amplitude in volts (0-10)
    float attackSamples;    // attack duration in samples
    float attackExponent;   // curve exponent for attack  (>1 = slow start, <1 = fast start)
    float holdSamples;      // hold duration in samples
    float releaseSamples;   // release duration in samples
    float releaseExponent;  // curve exponent for release (>1 = fast fall, <1 = slow fall)

    // DSP state
    float    phase;         // VCO phase [0, 1)
    float    envPos;        // position within current envelope segment (samples)
    float    prevTrig;      // previous trigger sample for rising edge detection
    EnvState envState;      // current envelope state

    BKICK()
        : freqInc        ( 60.0f / 48000.0f )
        , levelV         ( 10.0f )
        , attackSamples  ( 480.0f )     // 10 ms at 48 kHz
        , attackExponent ( 1.0f )       // linear (curve=50)
        , holdSamples    ( 2400.0f )    // 50 ms at 48 kHz
        , releaseSamples ( 24000.0f )   // 500 ms at 48 kHz
        , releaseExponent( 1.0f )       // linear (curve=50)
        , phase          ( 0.0f )
        , envPos         ( 0.0f )
        , prevTrig       ( 0.0f )
        , envState       ( kEnvIdle )
    {}
};

// ----------------------------------------------------------------------------
// Parameters
// ----------------------------------------------------------------------------

enum {
    kParamFreq,          // 0  VCO page
    kParamLevel,         // 1  VCO page
    kParamAttack,        // 2  AmpEnvelope page
    kParamAttackCurve,   // 3  AmpEnvelope page
    kParamHold,          // 4  AmpEnvelope page
    kParamRelease,       // 5  AmpEnvelope page
    kParamReleaseCurve,  // 6  AmpEnvelope page
    kParamTrigger,       // 7  AmpEnvelope page
    kParamOutput,        // 8  Output page
};

static const _NT_parameter parameters[] = {
    // VCO page
    { .name="Frequency",    .min=30, .max=200,  .def=60,  .unit=kNT_unitHz,      .scaling=0, .enumStrings=NULL },
    { .name="Level",        .min=0,  .max=100,  .def=100, .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    // AmpEnvelope page
    { .name="Attack",       .min=1,  .max=2000, .def=10,  .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    { .name="AttackCurve",  .min=0,  .max=100,  .def=50,  .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name="Hold",         .min=1,  .max=2000, .def=50,  .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    { .name="Release",      .min=1,  .max=2000, .def=500, .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    { .name="ReleaseCurve", .min=0,  .max=100,  .def=50,  .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    NT_PARAMETER_CV_INPUT( "Trigger", 1, 1 )
    // Output page
    NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )
};

// ----------------------------------------------------------------------------
// Parameter pages
// ----------------------------------------------------------------------------

static const uint8_t pageVCO[]    = { kParamFreq, kParamLevel };
static const uint8_t pageEnv[]    = { kParamAttack, kParamAttackCurve, kParamHold, kParamRelease, kParamReleaseCurve, kParamTrigger };
static const uint8_t pageOutput[] = { kParamOutput };

static const _NT_parameterPage pages[] = {
    { .name="VCO",         .numParams=2, .group=0, .params=pageVCO    },
    { .name="AmpEnvelope", .numParams=6, .group=0, .params=pageEnv    },
    { .name="Output",      .numParams=1, .group=0, .params=pageOutput },
};

static const _NT_parameterPages paramPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages    = pages,
};

// ----------------------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------------------

void calculateRequirements( _NT_algorithmRequirements& req, const int32_t* )
{
    req.numParameters = ARRAY_SIZE(parameters);
    req.sram = sizeof(BKICK);
    req.dram = 0;
    req.dtc  = 0;
    req.itc  = 0;
}

_NT_algorithm* construct( const _NT_algorithmMemoryPtrs& ptrs,
                           const _NT_algorithmRequirements&,
                           const int32_t* )
{
    BKICK* a = new (ptrs.sram) BKICK();
    a->parameters     = parameters;
    a->parameterPages = &paramPages;
    return a;
}

// ----------------------------------------------------------------------------
// parameterChanged
// Curve exponent formula:
//   exponent = kCurveBase ^ ((50 - curve) / 50)
//   curve=0  → exponent = kCurveBase   (e.g. 3) → t^3  slow start / fast fall
//   curve=50 → exponent = 1            → t^1  linear
//   curve=100→ exponent = 1/kCurveBase → t^(1/3) fast start / slow fall
// ----------------------------------------------------------------------------

void parameterChanged( _NT_algorithm* self, int p )
{
    BKICK* a = (BKICK*)self;
    float sr = NT_globals.sampleRate;

    switch ( p )
    {
    case kParamFreq:
        a->freqInc = a->v[kParamFreq] / sr;
        break;
    case kParamLevel:
        a->levelV = a->v[kParamLevel] / 100.0f * 10.0f;
        break;
    case kParamAttack:
        a->attackSamples = a->v[kParamAttack] * 0.001f * sr;
        break;
    case kParamAttackCurve:
        a->attackExponent = powf( kCurveBase, (50.0f - a->v[kParamAttackCurve]) / 50.0f );
        break;
    case kParamHold:
        a->holdSamples = a->v[kParamHold] * 0.001f * sr;
        break;
    case kParamRelease:
        a->releaseSamples = a->v[kParamRelease] * 0.001f * sr;
        break;
    case kParamReleaseCurve:
        a->releaseExponent = powf( kCurveBase, (50.0f - a->v[kParamReleaseCurve]) / 50.0f );
        break;
    }
}

// ----------------------------------------------------------------------------
// step
// ----------------------------------------------------------------------------

static const float kTwoPi = 6.28318530f;
static const float kPi    = 3.14159265f;

void step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
    BKICK* a = (BKICK*)self;
    int numFrames = numFramesBy4 * 4;

    const float* trig = busFrames + (a->v[kParamTrigger] - 1) * numFrames;
    float*       out  = busFrames + (a->v[kParamOutput]  - 1) * numFrames;

    float    phase           = a->phase;
    float    envPos          = a->envPos;
    float    prevTrig        = a->prevTrig;
    EnvState envState        = a->envState;

    const float freqInc        = a->freqInc;
    const float levelV         = a->levelV;
    const float attackSamples  = a->attackSamples;
    const float attackExponent = a->attackExponent;
    const float holdSamples    = a->holdSamples;
    const float releaseSamples = a->releaseSamples;
    const float releaseExponent= a->releaseExponent;

    for ( int i = 0; i < numFrames; ++i )
    {
        // Rising edge detection (threshold 1.0V)
        float t = trig[i];
        if ( prevTrig < 1.0f && t >= 1.0f )
        {
            phase    = 0.0f;    // reset VCO phase — sine starts at 0, rises positive
            envPos   = 0.0f;
            envState = kEnvAttack;
        }
        prevTrig = t;

        // Envelope state machine
        float env = 0.0f;
        switch ( envState )
        {
        case kEnvAttack:
        {
            float normalised = envPos / attackSamples;
            float curved     = powf( normalised, attackExponent );
            env = (1.0f - cosf( kPi * curved )) * 0.5f;
            envPos += 1.0f;
            if ( envPos >= attackSamples )
            {
                envPos   = 0.0f;
                envState = kEnvHold;
            }
            break;
        }
        case kEnvHold:
            env = 1.0f;
            envPos += 1.0f;
            if ( envPos >= holdSamples )
            {
                envPos   = 0.0f;
                envState = kEnvRelease;
            }
            break;

        case kEnvRelease:
        {
            float normalised = envPos / releaseSamples;
            float curved     = powf( normalised, releaseExponent );
            env = (1.0f + cosf( kPi * curved )) * 0.5f;
            envPos += 1.0f;
            if ( envPos >= releaseSamples )
            {
                envPos   = 0.0f;
                envState = kEnvIdle;
            }
            break;
        }
        case kEnvIdle:
        default:
            env = 0.0f;
            break;
        }

        // Sine VCO × level × envelope → output bus (Replace mode)
        out[i] = sinf( kTwoPi * phase ) * levelV * env;

        // Advance VCO phase
        phase += freqInc;
        if ( phase >= 1.0f ) phase -= 1.0f;
    }

    // Write state back
    a->phase    = phase;
    a->envPos   = envPos;
    a->prevTrig = prevTrig;
    a->envState = envState;
}

// ----------------------------------------------------------------------------
// Factory and entry point
// ----------------------------------------------------------------------------

static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('B','K','C','K'),
    .name                        = "BKICK",
    .description                 = "Bass kick drum: sine VCO with AHR amplitude envelope",
    .numSpecifications           = 0,
    .specifications              = NULL,
    .calculateStaticRequirements = NULL,
    .initialise                  = NULL,
    .calculateRequirements       = calculateRequirements,
    .construct                   = construct,
    .parameterChanged            = parameterChanged,
    .step                        = step,
    .draw                        = NULL,
    .midiRealtime                = NULL,
    .midiMessage                 = NULL,
    .tags                        = kNT_tagInstrument,
    .hasCustomUi                 = NULL,
    .customUi                    = NULL,
    .setupUi                     = NULL,
    .serialise                   = NULL,
    .deserialise                 = NULL,
    .midiSysEx                   = NULL,
    .parameterUiPrefix           = NULL,
    .parameterString             = NULL,
};

extern "C" uintptr_t pluginEntry( _NT_selector selector, uint32_t data )
{
    switch ( selector )
    {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return (uintptr_t)(data == 0 ? &factory : NULL);
    }
    return 0;
}
