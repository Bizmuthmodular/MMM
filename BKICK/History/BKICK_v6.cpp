#include "distingnt/api.h"
#include <cmath>
#include <new>

// ----------------------------------------------------------------------------
// Curve shaping
// Controls how much exponential/logarithmic curves deviate from linear.
// Increase for more dramatic curves, decrease for subtler shaping.
// ----------------------------------------------------------------------------

static const float kCurveBase = 3.0f;

// Maximum tanh drive at SatEnvAmount=100% and satEnv=1.
// At drive=10 the output is very close to a square wave.
// Increase for harder saturation, decrease for subtler maximum.
static const float kMaxDrive = 10.0f;

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
    // Cached derived values — Amp envelope
    float freqInc;              // VCO phase increment per sample (at base frequency)
    float levelV;               // peak amplitude in volts (0-10)
    float attackSamples;
    float attackExponent;
    float holdSamples;
    float releaseSamples;
    float releaseExponent;

    // Cached derived values — Pitch envelope
    float pitchAmount;
    float pitchDecaySamples;
    float pitchDecayExponent;
    uint8_t pitchDecaySlope;    // 0 = EXP, 1 = COS

    // Cached derived values — Sat envelope
    float satAttackSamples;
    float satAttackExponent;
    float satHoldSamples;
    float satReleaseSamples;
    float satReleaseExponent;
    float satEnvAmount;         // 0.0-1.0 (from v[]/100)

    // DSP state — Amp envelope
    float    phase;
    float    envPos;
    float    prevTrig;
    EnvState envState;

    // DSP state — Pitch envelope
    float   pitchEnvPos;
    uint8_t pitchEnvActive;

    // DSP state — Sat envelope
    float    satEnvPos;
    EnvState satEnvState;

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
        , satAttackSamples  ( 480.0f )
        , satAttackExponent ( 1.0f )
        , satHoldSamples    ( 2400.0f )
        , satReleaseSamples ( 24000.0f )
        , satReleaseExponent( 1.0f )
        , satEnvAmount      ( 0.0f )
        , phase             ( 0.0f )
        , envPos            ( 0.0f )
        , prevTrig          ( 0.0f )
        , envState          ( kEnvIdle )
        , pitchEnvPos       ( 0.0f )
        , pitchEnvActive    ( 0 )
        , satEnvPos         ( 0.0f )
        , satEnvState       ( kEnvIdle )
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
    kParamSatAttack,         // 12  SatEnvelope
    kParamSatAttackCurve,    // 13  SatEnvelope
    kParamSatHold,           // 14  SatEnvelope
    kParamSatRelease,        // 15  SatEnvelope
    kParamSatReleaseCurve,   // 16  SatEnvelope
    kParamSatEnvAmount,      // 17  SatEnvelope
    kParamOutput,            // 18  Output
};

static const _NT_parameter parameters[] = {
    // VCO
    { .name="Frequency",      .min=30, .max=200,  .def=60,  .unit=kNT_unitHz,       .scaling=0, .enumStrings=NULL        },
    { .name="Level",          .min=0,  .max=100,  .def=100, .unit=kNT_unitPercent,  .scaling=0, .enumStrings=NULL        },
    // AmpEnvelope
    { .name="Attack",         .min=1,  .max=2000, .def=10,  .unit=kNT_unitMs,       .scaling=0, .enumStrings=NULL        },
    { .name="AttackCurve",    .min=0,  .max=100,  .def=50,  .unit=kNT_unitPercent,  .scaling=0, .enumStrings=NULL        },
    { .name="Hold",           .min=1,  .max=2000, .def=50,  .unit=kNT_unitMs,       .scaling=0, .enumStrings=NULL        },
    { .name="Release",        .min=1,  .max=2000, .def=500, .unit=kNT_unitMs,       .scaling=0, .enumStrings=NULL        },
    { .name="ReleaseCurve",   .min=0,  .max=100,  .def=50,  .unit=kNT_unitPercent,  .scaling=0, .enumStrings=NULL        },
    NT_PARAMETER_CV_INPUT( "Trigger", 1, 1 )
    // PitchEnvelope
    { .name="Amount",         .min=0,  .max=48,   .def=0,   .unit=kNT_unitSemitones,.scaling=0, .enumStrings=NULL        },
    { .name="DecayTime",      .min=1,  .max=1000, .def=200, .unit=kNT_unitMs,       .scaling=0, .enumStrings=NULL        },
    { .name="DecayCurve",     .min=0,  .max=100,  .def=50,  .unit=kNT_unitPercent,  .scaling=0, .enumStrings=NULL        },
    { .name="DecaySlope",     .min=0,  .max=1,    .def=0,   .unit=kNT_unitEnum,     .scaling=0, .enumStrings=slopeStrings },
    // SatEnvelope
    { .name="Attack",         .min=1,  .max=2000, .def=10,  .unit=kNT_unitMs,       .scaling=0, .enumStrings=NULL        },
    { .name="AttackCurve",    .min=0,  .max=100,  .def=50,  .unit=kNT_unitPercent,  .scaling=0, .enumStrings=NULL        },
    { .name="Hold",           .min=1,  .max=2000, .def=50,  .unit=kNT_unitMs,       .scaling=0, .enumStrings=NULL        },
    { .name="Release",        .min=1,  .max=2000, .def=500, .unit=kNT_unitMs,       .scaling=0, .enumStrings=NULL        },
    { .name="ReleaseCurve",   .min=0,  .max=100,  .def=50,  .unit=kNT_unitPercent,  .scaling=0, .enumStrings=NULL        },
    { .name="SatEnvAmount",   .min=0,  .max=100,  .def=0,   .unit=kNT_unitPercent,  .scaling=0, .enumStrings=NULL        },
    // Output
    NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )
};

// ----------------------------------------------------------------------------
// Parameter pages
// ----------------------------------------------------------------------------

static const uint8_t pageVCO[]      = { kParamFreq, kParamLevel };
static const uint8_t pageAmpEnv[]   = { kParamAttack, kParamAttackCurve, kParamHold, kParamRelease, kParamReleaseCurve, kParamTrigger };
static const uint8_t pagePitchEnv[] = { kParamPitchAmount, kParamPitchDecayTime, kParamPitchDecayCurve, kParamPitchDecaySlope };
static const uint8_t pageSatEnv[]   = { kParamSatAttack, kParamSatAttackCurve, kParamSatHold, kParamSatRelease, kParamSatReleaseCurve, kParamSatEnvAmount };
static const uint8_t pageOutput[]   = { kParamOutput };

static const _NT_parameterPage pages[] = {
    { .name="VCO",           .numParams=2, .group=0, .params=pageVCO      },
    { .name="AmpEnvelope",   .numParams=6, .group=0, .params=pageAmpEnv   },
    { .name="PitchEnvelope", .numParams=4, .group=0, .params=pagePitchEnv },
    { .name="SatEnvelope",   .numParams=6, .group=0, .params=pageSatEnv   },
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
    case kParamSatAttack:
        a->satAttackSamples = a->v[kParamSatAttack] * 0.001f * sr;
        break;
    case kParamSatAttackCurve:
        a->satAttackExponent = powf( kCurveBase, (50.0f - a->v[kParamSatAttackCurve]) / 50.0f );
        break;
    case kParamSatHold:
        a->satHoldSamples = a->v[kParamSatHold] * 0.001f * sr;
        break;
    case kParamSatRelease:
        a->satReleaseSamples = a->v[kParamSatRelease] * 0.001f * sr;
        break;
    case kParamSatReleaseCurve:
        a->satReleaseExponent = powf( kCurveBase, (50.0f - a->v[kParamSatReleaseCurve]) / 50.0f );
        break;
    case kParamSatEnvAmount:
        a->satEnvAmount = a->v[kParamSatEnvAmount] / 100.0f;
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
    float    envPos         = a->envPos;
    float    prevTrig       = a->prevTrig;
    EnvState envState       = a->envState;
    float    pitchEnvPos    = a->pitchEnvPos;
    uint8_t  pitchEnvActive = a->pitchEnvActive;
    float    satEnvPos      = a->satEnvPos;
    EnvState satEnvState    = a->satEnvState;

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
    const float satAttackSamples  = a->satAttackSamples;
    const float satAttackExp      = a->satAttackExponent;
    const float satHoldSamples    = a->satHoldSamples;
    const float satReleaseSamples = a->satReleaseSamples;
    const float satReleaseExp     = a->satReleaseExponent;
    const float satEnvAmount      = a->satEnvAmount;

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
            satEnvPos      = 0.0f;
            satEnvState    = kEnvAttack;
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

        // Sat envelope
        float satEnv = 0.0f;
        switch ( satEnvState )
        {
        case kEnvAttack:
        {
            float normalised = satEnvPos / satAttackSamples;
            float curved     = powf( normalised, satAttackExp );
            satEnv = (1.0f - cosf( kPi * curved )) * 0.5f;
            satEnvPos += 1.0f;
            if ( satEnvPos >= satAttackSamples ) { satEnvPos = 0.0f; satEnvState = kEnvHold; }
            break;
        }
        case kEnvHold:
            satEnv = 1.0f;
            satEnvPos += 1.0f;
            if ( satEnvPos >= satHoldSamples ) { satEnvPos = 0.0f; satEnvState = kEnvRelease; }
            break;
        case kEnvRelease:
        {
            float normalised = satEnvPos / satReleaseSamples;
            float curved     = powf( normalised, satReleaseExp );
            satEnv = (1.0f + cosf( kPi * curved )) * 0.5f;
            satEnvPos += 1.0f;
            if ( satEnvPos >= satReleaseSamples ) { satEnvPos = 0.0f; satEnvState = kEnvIdle; }
            break;
        }
        case kEnvIdle:
        default:
            satEnv = 0.0f;
            break;
        }

        // VCO: sine wave
        float currentFreqInc = freqInc * exp2f( pitchSemitones / 12.0f );
        float sineVal        = sinf( kTwoPi * phase );

        // Tanh saturation: drive=0 → pure sine, drive=kMaxDrive → near square
        // tanhf(drive × x) / tanhf(drive) normalises output to ±1 at any drive.
        // Guard against drive≈0 (division by near-zero tanhf(drive)).
        float drive     = satEnv * satEnvAmount * kMaxDrive;
        float saturated = (drive < 0.001f) ? sineVal : tanhf( drive * sineVal ) / tanhf( drive );

        out[i] = saturated * levelV * env;

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
    a->satEnvPos      = satEnvPos;
    a->satEnvState    = satEnvState;
}

// ----------------------------------------------------------------------------
// Factory and entry point
// ----------------------------------------------------------------------------

static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('B','K','C','K'),
    .name                        = "BKICK",
    .description                 = "Bass kick drum: sine VCO with AHR amp, pitch, and saturation envelopes",
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
