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
    float freqInc;              // VCO phase increment per sample (at base frequency)
    float levelV;               // peak amplitude in volts (0-10)
    float attackSamples;        // attack duration in samples
    float attackExponent;       // curve exponent for attack
    float holdSamples;          // hold duration in samples
    float releaseSamples;       // release duration in samples
    float releaseExponent;      // curve exponent for release
    float pitchAmount;          // pitch envelope start offset in semitones
    float pitchDecaySamples;    // pitch decay duration in samples
    float pitchDecayExponent;   // curve exponent for pitch decay
    uint8_t pitchDecaySlope;    // 0 = EXP, 1 = COS

    // DSP state
    float    phase;             // VCO phase [0, 1)
    float    envPos;            // position within current amp envelope segment (samples)
    float    prevTrig;          // previous trigger sample for rising edge detection
    EnvState envState;          // current amp envelope state
    float    pitchEnvPos;       // position within pitch decay (samples)
    uint8_t  pitchEnvActive;    // 1 if pitch decay is running, 0 if idle

    BKICK()
        : freqInc           ( 60.0f / 48000.0f )
        , levelV            ( 10.0f )
        , attackSamples     ( 480.0f )      // 10 ms at 48 kHz
        , attackExponent    ( 1.0f )        // linear (curve=50)
        , holdSamples       ( 2400.0f )     // 50 ms at 48 kHz
        , releaseSamples    ( 24000.0f )    // 500 ms at 48 kHz
        , releaseExponent   ( 1.0f )        // linear (curve=50)
        , pitchAmount       ( 0.0f )        // 0 semitones (no pitch env)
        , pitchDecaySamples ( 9600.0f )     // 200 ms at 48 kHz
        , pitchDecayExponent( 1.0f )        // linear (curve=50)
        , pitchDecaySlope   ( 0 )           // EXP
        , phase             ( 0.0f )
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
    kParamFreq,              //  0  VCO page
    kParamLevel,             //  1  VCO page
    kParamAttack,            //  2  AmpEnvelope page
    kParamAttackCurve,       //  3  AmpEnvelope page
    kParamHold,              //  4  AmpEnvelope page
    kParamRelease,           //  5  AmpEnvelope page
    kParamReleaseCurve,      //  6  AmpEnvelope page
    kParamTrigger,           //  7  AmpEnvelope page
    kParamPitchAmount,       //  8  PitchEnvelope page
    kParamPitchDecayTime,    //  9  PitchEnvelope page
    kParamPitchDecayCurve,   // 10  PitchEnvelope page
    kParamPitchDecaySlope,   // 11  PitchEnvelope page
    kParamOutput,            // 12  Output page
};

static const _NT_parameter parameters[] = {
    // VCO page
    { .name="Frequency",      .min=30, .max=200,  .def=60,  .unit=kNT_unitHz,       .scaling=0, .enumStrings=NULL         },
    { .name="Level",          .min=0,  .max=100,  .def=100, .unit=kNT_unitPercent,  .scaling=0, .enumStrings=NULL         },
    // AmpEnvelope page
    { .name="Attack",         .min=1,  .max=2000, .def=10,  .unit=kNT_unitMs,       .scaling=0, .enumStrings=NULL         },
    { .name="AttackCurve",    .min=0,  .max=100,  .def=50,  .unit=kNT_unitPercent,  .scaling=0, .enumStrings=NULL         },
    { .name="Hold",           .min=1,  .max=2000, .def=50,  .unit=kNT_unitMs,       .scaling=0, .enumStrings=NULL         },
    { .name="Release",        .min=1,  .max=2000, .def=500, .unit=kNT_unitMs,       .scaling=0, .enumStrings=NULL         },
    { .name="ReleaseCurve",   .min=0,  .max=100,  .def=50,  .unit=kNT_unitPercent,  .scaling=0, .enumStrings=NULL         },
    NT_PARAMETER_CV_INPUT( "Trigger", 1, 1 )
    // PitchEnvelope page
    { .name="Amount",         .min=0,  .max=48,   .def=0,   .unit=kNT_unitSemitones,.scaling=0, .enumStrings=NULL         },
    { .name="DecayTime",      .min=1,  .max=1000, .def=200, .unit=kNT_unitMs,       .scaling=0, .enumStrings=NULL         },
    { .name="DecayCurve",     .min=0,  .max=100,  .def=50,  .unit=kNT_unitPercent,  .scaling=0, .enumStrings=NULL         },
    { .name="DecaySlope",     .min=0,  .max=1,    .def=0,   .unit=kNT_unitEnum,     .scaling=0, .enumStrings=slopeStrings  },
    // Output page
    NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )
};

// ----------------------------------------------------------------------------
// Parameter pages
// ----------------------------------------------------------------------------

static const uint8_t pageVCO[]        = { kParamFreq, kParamLevel };
static const uint8_t pageAmpEnv[]     = { kParamAttack, kParamAttackCurve, kParamHold, kParamRelease, kParamReleaseCurve, kParamTrigger };
static const uint8_t pagePitchEnv[]   = { kParamPitchAmount, kParamPitchDecayTime, kParamPitchDecayCurve, kParamPitchDecaySlope };
static const uint8_t pageOutput[]     = { kParamOutput };

static const _NT_parameterPage pages[] = {
    { .name="VCO",           .numParams=2, .group=0, .params=pageVCO      },
    { .name="AmpEnvelope",   .numParams=6, .group=0, .params=pageAmpEnv   },
    { .name="PitchEnvelope", .numParams=4, .group=0, .params=pagePitchEnv },
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

    float    phase             = a->phase;
    float    envPos            = a->envPos;
    float    prevTrig          = a->prevTrig;
    EnvState envState          = a->envState;
    float    pitchEnvPos       = a->pitchEnvPos;
    uint8_t  pitchEnvActive    = a->pitchEnvActive;

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

    for ( int i = 0; i < numFrames; ++i )
    {
        // Rising edge detection (threshold 1.0V)
        float t = trig[i];
        if ( prevTrig < 1.0f && t >= 1.0f )
        {
            phase          = 0.0f;
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
                pitchSemitones = pitchAmount * (1.0f - curved);                     // EXP
            else
                pitchSemitones = pitchAmount * (1.0f + cosf( kPi * curved )) * 0.5f; // COS

            pitchEnvPos += 1.0f;
            if ( pitchEnvPos >= pitchDecaySamples )
            {
                pitchEnvPos    = 0.0f;
                pitchEnvActive = 0;
            }
        }

        // Amp envelope state machine
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

        // VCO: sine at base frequency shifted by pitch envelope
        // exp2f(semitones/12) converts semitone offset to frequency ratio
        float currentFreqInc = freqInc * exp2f( pitchSemitones / 12.0f );
        out[i] = sinf( kTwoPi * phase ) * levelV * env;

        phase += currentFreqInc;
        if ( phase >= 1.0f ) phase -= 1.0f;
    }

    // Write state back
    a->phase          = phase;
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
    .description                 = "Bass kick drum: sine VCO with AHR amp envelope and pitch envelope",
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
