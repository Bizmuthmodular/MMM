#include "distingnt/api.h"
#include <cmath>
#include <new>

// ----------------------------------------------------------------------------
// Tunable constants
// ----------------------------------------------------------------------------

// Controls how much envelope/logarithmic curves deviate from linear.
static const float kCurveBase = 3.0f;

// Maximum tanh drive at Saturation=100. Each of the 4 stages uses a
// progressively reduced drive (×0.8 per stage) to build rich harmonics
// without immediately collapsing to a square wave.
static const float kMaxDrive  = 50.0f;

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
// Saturation helpers
// ----------------------------------------------------------------------------

// Single tanh stage normalised to ±1 output range.
static inline float tanhStage( float x, float d )
{
    if ( d < 0.001f ) return x;
    return tanhf( d * x ) / tanhf( d );
}

// 4-stage tanh saturator with DC bias.
// Stage 1 applies the bias (adds even harmonics), DC-compensates its output,
// then stages 2-4 operate symmetrically (add odd harmonics with different character).
// Drive is reduced by 0.8x per stage for a richer harmonic texture.
static inline float saturate4( float x, float drive, float bias )
{
    if ( drive < 0.001f ) return x;

    const float d1 = drive;
    const float d2 = drive * 0.800f;
    const float d3 = drive * 0.640f;
    const float d4 = drive * 0.512f;

    // Stage 1: biased input, DC offset removed
    float s1 = tanhStage( x + bias, d1 ) - tanhStage( bias, d1 );
    // Stages 2-4: symmetric
    float s2 = tanhStage( s1, d2 );
    float s3 = tanhStage( s2, d3 );
    float s4 = tanhStage( s3, d4 );

    return s4;
}

// 4x oversampled saturation using linear interpolation between consecutive
// input samples. The 4 sub-samples are saturated and averaged — averaging
// acts as a first-order anti-aliasing filter, removing folded harmonics above Nyquist.
static inline float saturateOversampled( float x, float prevX, float drive, float bias )
{
    float acc = 0.0f;
    for ( int sub = 0; sub < 4; ++sub )
    {
        float subSample = prevX + (x - prevX) * (sub * 0.25f);
        acc += saturate4( subSample, drive, bias );
    }
    return acc * 0.25f;
}

// ----------------------------------------------------------------------------
// Algorithm struct
// ----------------------------------------------------------------------------

struct BKICK : public _NT_algorithm
{
    // Cached — VCO / Amp envelope
    float freqInc;
    float levelV;
    float attackSamples;
    float attackExponent;
    float holdSamples;
    float releaseSamples;
    float releaseExponent;

    // Cached — Pitch envelope
    float   pitchAmount;
    float   pitchDecaySamples;
    float   pitchDecayExponent;
    uint8_t pitchDecaySlope;    // 0=EXP, 1=COS

    // Cached — Saturator
    float satDrive;             // 0 to kMaxDrive
    float satBias;              // -0.5 to +0.5 (normalised signal units)
    float satDryWet;            // 0.0 to 1.0

    // DSP state — Amp envelope
    float    phase;
    float    prevSine;          // previous sample's sine value (for oversampling)
    float    envPos;
    float    prevTrig;
    EnvState envState;

    // DSP state — Pitch envelope
    float   pitchEnvPos;
    uint8_t pitchEnvActive;

    BKICK()
        : freqInc           ( 60.0f / 48000.0f )
        , levelV            ( 10.0f )
        , attackSamples     ( 480.0f )
        , attackExponent    ( 1.0f )
        , holdSamples       ( 2400.0f )
        , releaseSamples    ( 24000.0f )
        , releaseExponent   ( 1.0f )
        , pitchAmount       ( 0.0f )
        , pitchDecaySamples ( 9600.0f )
        , pitchDecayExponent( 1.0f )
        , pitchDecaySlope   ( 0 )
        , satDrive          ( 0.0f )
        , satBias           ( 0.0f )
        , satDryWet         ( 0.0f )
        , phase             ( 0.0f )
        , prevSine          ( 0.0f )
        , envPos            ( 0.0f )
        , prevTrig          ( 0.0f )
        , envState          ( kEnvIdle )
        , pitchEnvPos       ( 0.0f )
        , pitchEnvActive    ( 0 )
    {}
};

// ----------------------------------------------------------------------------
// Parameters
// ----------------------------------------------------------------------------

static const char* const slopeStrings[] = { "EXP", "COS", NULL };

enum {
    kParamFreq,              //  0  VCO
    kParamLevel,             //  1  VCO
    kParamAttack,            //  2  AmpEnvelope
    kParamAttackCurve,       //  3  AmpEnvelope
    kParamHold,              //  4  AmpEnvelope
    kParamRelease,           //  5  AmpEnvelope
    kParamReleaseCurve,      //  6  AmpEnvelope
    kParamTrigger,           //  7  AmpEnvelope
    kParamPitchAmount,       //  8  PitchEnvelope
    kParamPitchDecayTime,    //  9  PitchEnvelope
    kParamPitchDecayCurve,   // 10  PitchEnvelope
    kParamPitchDecaySlope,   // 11  PitchEnvelope
    kParamSaturation,        // 12  Saturator
    kParamBias,              // 13  Saturator
    kParamDryWet,            // 14  Saturator
    kParamOutput,            // 15  Output
};

static const _NT_parameter parameters[] = {
    // VCO
    { .name="Frequency",    .min=30,  .max=200,  .def=60,  .unit=kNT_unitHz,       .scaling=0,              .enumStrings=NULL        },
    { .name="Level",        .min=0,   .max=100,  .def=100, .unit=kNT_unitPercent,  .scaling=0,              .enumStrings=NULL        },
    // AmpEnvelope
    { .name="Attack",       .min=1,   .max=2000, .def=10,  .unit=kNT_unitMs,       .scaling=0,              .enumStrings=NULL        },
    { .name="AttackCurve",  .min=0,   .max=100,  .def=50,  .unit=kNT_unitPercent,  .scaling=0,              .enumStrings=NULL        },
    { .name="Hold",         .min=1,   .max=2000, .def=50,  .unit=kNT_unitMs,       .scaling=0,              .enumStrings=NULL        },
    { .name="Release",      .min=1,   .max=2000, .def=500, .unit=kNT_unitMs,       .scaling=0,              .enumStrings=NULL        },
    { .name="ReleaseCurve", .min=0,   .max=100,  .def=50,  .unit=kNT_unitPercent,  .scaling=0,              .enumStrings=NULL        },
    NT_PARAMETER_CV_INPUT( "Trigger", 1, 1 )
    // PitchEnvelope
    { .name="Amount",       .min=0,   .max=48,   .def=0,   .unit=kNT_unitSemitones,.scaling=0,              .enumStrings=NULL        },
    { .name="DecayTime",    .min=1,   .max=1000, .def=200, .unit=kNT_unitMs,       .scaling=0,              .enumStrings=NULL        },
    { .name="DecayCurve",   .min=0,   .max=100,  .def=50,  .unit=kNT_unitPercent,  .scaling=0,              .enumStrings=NULL        },
    { .name="DecaySlope",   .min=0,   .max=1,    .def=0,   .unit=kNT_unitEnum,     .scaling=0,              .enumStrings=slopeStrings },
    // Saturator
    { .name="Saturation",   .min=0,   .max=100,  .def=0,   .unit=kNT_unitPercent,  .scaling=0,              .enumStrings=NULL        },
    { .name="Bias",         .min=-50, .max=50,   .def=0,   .unit=kNT_unitVolts,    .scaling=kNT_scaling10,  .enumStrings=NULL        },
    { .name="Dry/Wet",      .min=0,   .max=100,  .def=0,   .unit=kNT_unitPercent,  .scaling=0,              .enumStrings=NULL        },
    // Output
    NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )
};

// ----------------------------------------------------------------------------
// Parameter pages
// ----------------------------------------------------------------------------

static const uint8_t pageVCO[]      = { kParamFreq, kParamLevel };
static const uint8_t pageAmpEnv[]   = { kParamAttack, kParamAttackCurve, kParamHold, kParamRelease, kParamReleaseCurve, kParamTrigger };
static const uint8_t pagePitchEnv[] = { kParamPitchAmount, kParamPitchDecayTime, kParamPitchDecayCurve, kParamPitchDecaySlope };
static const uint8_t pageSat[]      = { kParamSaturation, kParamBias, kParamDryWet };
static const uint8_t pageOutput[]   = { kParamOutput };

static const _NT_parameterPage pages[] = {
    { .name="VCO",           .numParams=2, .group=0, .params=pageVCO      },
    { .name="AmpEnvelope",   .numParams=6, .group=0, .params=pageAmpEnv   },
    { .name="PitchEnvelope", .numParams=4, .group=0, .params=pagePitchEnv },
    { .name="Saturator",     .numParams=3, .group=0, .params=pageSat      },
    { .name="Output",        .numParams=1, .group=0, .params=pageOutput   },
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
//   curve=0   → exponent = kCurveBase (3) → slow start / fast fall
//   curve=50  → exponent = 1              → linear
//   curve=100 → exponent = 1/kCurveBase   → fast start / slow fall
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
    case kParamPitchAmount:
        a->pitchAmount = (float)a->v[kParamPitchAmount];
        break;
    case kParamPitchDecayTime:
        a->pitchDecaySamples = a->v[kParamPitchDecayTime] * 0.001f * sr;
        break;
    case kParamPitchDecayCurve:
        a->pitchDecayExponent = powf( kCurveBase, (50.0f - a->v[kParamPitchDecayCurve]) / 50.0f );
        break;
    case kParamPitchDecaySlope:
        a->pitchDecaySlope = (uint8_t)a->v[kParamPitchDecaySlope];
        break;
    case kParamSaturation:
        a->satDrive = a->v[kParamSaturation] / 100.0f * kMaxDrive;
        break;
    case kParamBias:
        // Raw -50 to +50 (displays as ±5.0V) → normalised ±0.5
        a->satBias = a->v[kParamBias] / 100.0f;
        break;
    case kParamDryWet:
        a->satDryWet = a->v[kParamDryWet] / 100.0f;
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

    float    phase          = a->phase;
    float    prevSine       = a->prevSine;
    float    envPos         = a->envPos;
    float    prevTrig       = a->prevTrig;
    EnvState envState       = a->envState;
    float    pitchEnvPos    = a->pitchEnvPos;
    uint8_t  pitchEnvActive = a->pitchEnvActive;

    const float freqInc           = a->freqInc;
    const float levelV            = a->levelV;
    const float attackSamples     = a->attackSamples;
    const float attackExponent    = a->attackExponent;
    const float holdSamples       = a->holdSamples;
    const float releaseSamples    = a->releaseSamples;
    const float releaseExponent   = a->releaseExponent;
    const float pitchAmount       = a->pitchAmount;
    const float pitchDecaySamples = a->pitchDecaySamples;
    const float pitchDecayExp     = a->pitchDecayExponent;
    const uint8_t pitchSlope      = a->pitchDecaySlope;
    const float satDrive          = a->satDrive;
    const float satBias           = a->satBias;
    const float satDryWet         = a->satDryWet;

    // Equal power dry/wet coefficients (constant per block)
    const float wetCoeff = sinf( satDryWet * kPi * 0.5f );
    const float dryCoeff = cosf( satDryWet * kPi * 0.5f );

    for ( int i = 0; i < numFrames; ++i )
    {
        // Rising edge detection (threshold 1.0V)
        float t = trig[i];
        if ( prevTrig < 1.0f && t >= 1.0f )
        {
            phase          = 0.0f;
            prevSine       = 0.0f;
            envPos         = 0.0f;
            envState       = kEnvAttack;
            pitchEnvPos    = 0.0f;
            pitchEnvActive = 1;
        }
        prevTrig = t;

        // Pitch envelope — decays from pitchAmount semitones to 0
        float pitchSemitones = 0.0f;
        if ( pitchEnvActive )
        {
            float normalised = pitchEnvPos / pitchDecaySamples;
            float curved     = powf( normalised, pitchDecayExp );
            if ( pitchSlope == 0 )
                pitchSemitones = pitchAmount * (1.0f - curved);
            else
                pitchSemitones = pitchAmount * (1.0f + cosf( kPi * curved )) * 0.5f;

            pitchEnvPos += 1.0f;
            if ( pitchEnvPos >= pitchDecaySamples )
            {
                pitchEnvPos    = 0.0f;
                pitchEnvActive = 0;
            }
        }

        // Amp envelope
        float env = 0.0f;
        switch ( envState )
        {
        case kEnvAttack:
        {
            float normalised = envPos / attackSamples;
            float curved     = powf( normalised, attackExponent );
            env = (1.0f - cosf( kPi * curved )) * 0.5f;
            envPos += 1.0f;
            if ( envPos >= attackSamples ) { envPos = 0.0f; envState = kEnvHold; }
            break;
        }
        case kEnvHold:
            env = 1.0f;
            envPos += 1.0f;
            if ( envPos >= holdSamples ) { envPos = 0.0f; envState = kEnvRelease; }
            break;
        case kEnvRelease:
        {
            float normalised = envPos / releaseSamples;
            float curved     = powf( normalised, releaseExponent );
            env = (1.0f + cosf( kPi * curved )) * 0.5f;
            envPos += 1.0f;
            if ( envPos >= releaseSamples ) { envPos = 0.0f; envState = kEnvIdle; }
            break;
        }
        case kEnvIdle:
        default:
            env = 0.0f;
            break;
        }

        // VCO
        float currentFreqInc = freqInc * exp2f( pitchSemitones / 12.0f );
        float sineVal        = sinf( kTwoPi * phase );

        // 4x oversampled 4-stage tanh saturator with DC bias
        float saturated = saturateOversampled( sineVal, prevSine, satDrive, satBias );

        // Equal power dry/wet crossfade, then amp envelope and level
        out[i] = (dryCoeff * sineVal + wetCoeff * saturated) * levelV * env;

        prevSine = sineVal;
        phase += currentFreqInc;
        if ( phase >= 1.0f ) phase -= 1.0f;
    }

    // Write state back
    a->phase          = phase;
    a->prevSine       = prevSine;
    a->envPos         = envPos;
    a->prevTrig       = prevTrig;
    a->envState       = envState;
    a->pitchEnvPos    = pitchEnvPos;
    a->pitchEnvActive = pitchEnvActive;
}

// ----------------------------------------------------------------------------
// Factory and entry point
// ----------------------------------------------------------------------------

static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('B','K','C','K'),
    .name                        = "BKICK",
    .description                 = "Bass kick drum: sine VCO, AHR amp/pitch envelopes, 4-stage oversampled saturator",
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
