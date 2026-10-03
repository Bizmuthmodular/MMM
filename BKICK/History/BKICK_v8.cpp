#include "distingnt/api.h"
#include <cmath>
#include <new>

// ----------------------------------------------------------------------------
// Tunable constants
// ----------------------------------------------------------------------------

// Controls how much curve parameters deviate from linear.
static const float kCurveBase = 3.0f;

// Maximum tanh drive at Saturation=100%.
static const float kMaxDrive  = 50.0f;

// Voltage scale used for saturation normalisation.
// Saturation operates on the signal normalised to ±1 (dividing by kSatScale),
// then scales back. Set to the maximum expected post-VCA signal amplitude.
static const float kSatScale  = 10.0f;

static const float kTwoPi     = 6.28318530f;
static const float kPi        = 3.14159265f;

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
// Biquad bandpass coefficient computation
// Computes normalised direct-form II transposed biquad coefficients.
// For a bandpass filter: b1=0, b2=-b0 (implied — only b0, a1, a2 stored).
// ----------------------------------------------------------------------------

static void computeBP( float& b0, float& a1, float& a2,
                        float fc, float bw, float sr )
{
    // Clamp to valid operating range
    if ( fc < 30.0f    ) fc = 30.0f;
    if ( fc > 15000.0f ) fc = 15000.0f;
    if ( bw < 1.0f     ) bw = 1.0f;

    float Q = fc / bw;
    if ( Q < 0.1f ) Q = 0.1f;

    float w0    = kTwoPi * fc / sr;
    float alpha = sinf( w0 ) / ( 2.0f * Q );
    float cosw0 = cosf( w0 );
    float a0inv = 1.0f / ( 1.0f + alpha );

    b0 =  alpha       * a0inv;  // b2 = -b0 (implied), b1 = 0 (implied)
    a1 = -2.0f * cosw0 * a0inv;
    a2 = ( 1.0f - alpha ) * a0inv;
}

// ----------------------------------------------------------------------------
// Algorithm struct
// ----------------------------------------------------------------------------

struct BKICK : public _NT_algorithm
{
    // Cached — VCO / Amp envelope
    float freqInc;
    float levelV;
    float attackSamples,  attackExponent;
    float holdSamples;
    float releaseSamples, releaseExponent;

    // Cached — Pitch envelope
    float   pitchAmount, pitchDecaySamples, pitchDecayExponent;
    uint8_t pitchDecaySlope;    // 0=EXP, 1=COS

    // Cached — Saturator
    float satDrive;             // 0 to kMaxDrive
    float satBias;              // normalised ±0.5
    float satDryWet;            // 0.0-1.0
    float satFeedback;          // 0.0-1.0

    // Cached — Bandpass filter coefficients (3 parallel filters)
    float bpB0[3], bpA1[3], bpA2[3];   // b2=-b0 (implicit), b1=0 (implicit)

    // DSP state — Amp envelope
    float    phase, envPos, prevTrig;
    EnvState envState;

    // DSP state — Pitch envelope
    float   pitchEnvPos;
    uint8_t pitchEnvActive;

    // DSP state — Biquad filters
    float bpX1[3], bpX2[3], bpY1[3], bpY2[3];

    // DSP state — Feedback memory
    float prevBpOut;

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
        , satFeedback       ( 0.0f )
        , phase             ( 0.0f )
        , envPos            ( 0.0f )
        , prevTrig          ( 0.0f )
        , envState          ( kEnvIdle )
        , pitchEnvPos       ( 0.0f )
        , pitchEnvActive    ( 0 )
        , prevBpOut         ( 0.0f )
    {
        for ( int i = 0; i < 3; ++i )
        {
            bpB0[i] = 0.0f; bpA1[i] = 0.0f; bpA2[i] = 0.0f;
            bpX1[i] = 0.0f; bpX2[i] = 0.0f;
            bpY1[i] = 0.0f; bpY2[i] = 0.0f;
        }
    }
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
    kParamBaseFreq,          // 15  Saturator
    kParamSpacing,           // 16  Saturator
    kParamBandwidth,         // 17  Saturator
    kParamFeedback,          // 18  Saturator
    kParamOutput,            // 19  Output
};

static const _NT_parameter parameters[] = {
    // VCO
    { .name="Frequency",    .min=30,    .max=200,   .def=60,  .unit=kNT_unitHz,       .scaling=0,             .enumStrings=NULL        },
    { .name="Level",        .min=0,     .max=100,   .def=100, .unit=kNT_unitPercent,  .scaling=0,             .enumStrings=NULL        },
    // AmpEnvelope
    { .name="Attack",       .min=1,     .max=2000,  .def=10,  .unit=kNT_unitMs,       .scaling=0,             .enumStrings=NULL        },
    { .name="AttackCurve",  .min=0,     .max=100,   .def=50,  .unit=kNT_unitPercent,  .scaling=0,             .enumStrings=NULL        },
    { .name="Hold",         .min=1,     .max=2000,  .def=50,  .unit=kNT_unitMs,       .scaling=0,             .enumStrings=NULL        },
    { .name="Release",      .min=1,     .max=2000,  .def=500, .unit=kNT_unitMs,       .scaling=0,             .enumStrings=NULL        },
    { .name="ReleaseCurve", .min=0,     .max=100,   .def=50,  .unit=kNT_unitPercent,  .scaling=0,             .enumStrings=NULL        },
    NT_PARAMETER_CV_INPUT( "Trigger", 1, 1 )
    // PitchEnvelope
    { .name="Amount",       .min=0,     .max=48,    .def=0,   .unit=kNT_unitSemitones,.scaling=0,             .enumStrings=NULL        },
    { .name="DecayTime",    .min=1,     .max=1000,  .def=200, .unit=kNT_unitMs,       .scaling=0,             .enumStrings=NULL        },
    { .name="DecayCurve",   .min=0,     .max=100,   .def=50,  .unit=kNT_unitPercent,  .scaling=0,             .enumStrings=NULL        },
    { .name="DecaySlope",   .min=0,     .max=1,     .def=0,   .unit=kNT_unitEnum,     .scaling=0,             .enumStrings=slopeStrings },
    // Saturator
    { .name="Saturation",   .min=0,     .max=100,   .def=0,   .unit=kNT_unitPercent,  .scaling=0,             .enumStrings=NULL        },
    { .name="Bias",         .min=-50,   .max=50,    .def=0,   .unit=kNT_unitVolts,    .scaling=kNT_scaling10, .enumStrings=NULL        },
    { .name="Dry/Wet",      .min=0,     .max=100,   .def=0,   .unit=kNT_unitPercent,  .scaling=0,             .enumStrings=NULL        },
    { .name="BaseFreq",     .min=30,    .max=15000, .def=80,  .unit=kNT_unitHz,       .scaling=0,             .enumStrings=NULL        },
    { .name="Spacing",      .min=0,     .max=15000, .def=0,   .unit=kNT_unitHz,       .scaling=0,             .enumStrings=NULL        },
    { .name="Bandwidth",    .min=10,    .max=1000,  .def=100, .unit=kNT_unitHz,       .scaling=0,             .enumStrings=NULL        },
    { .name="Feedback",     .min=0,     .max=100,   .def=0,   .unit=kNT_unitPercent,  .scaling=0,             .enumStrings=NULL        },
    // Output
    NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )
};

// ----------------------------------------------------------------------------
// Parameter pages
// ----------------------------------------------------------------------------

static const uint8_t pageVCO[]      = { kParamFreq, kParamLevel };
static const uint8_t pageAmpEnv[]   = { kParamAttack, kParamAttackCurve, kParamHold, kParamRelease, kParamReleaseCurve, kParamTrigger };
static const uint8_t pagePitchEnv[] = { kParamPitchAmount, kParamPitchDecayTime, kParamPitchDecayCurve, kParamPitchDecaySlope };
static const uint8_t pageSat[]      = { kParamSaturation, kParamBias, kParamDryWet, kParamBaseFreq, kParamSpacing, kParamBandwidth, kParamFeedback };
static const uint8_t pageOutput[]   = { kParamOutput };

static const _NT_parameterPage pages[] = {
    { .name="VCO",           .numParams=2, .group=0, .params=pageVCO      },
    { .name="AmpEnvelope",   .numParams=6, .group=0, .params=pageAmpEnv   },
    { .name="PitchEnvelope", .numParams=4, .group=0, .params=pagePitchEnv },
    { .name="Saturator",     .numParams=7, .group=0, .params=pageSat      },
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
// BP filter coefficient update helper
// Called when BaseFreq, Spacing, or Bandwidth changes.
// ----------------------------------------------------------------------------

static void updateBPFilters( BKICK* a )
{
    float sr      = NT_globals.sampleRate;
    float base    = (float)a->v[kParamBaseFreq];
    float spacing = (float)a->v[kParamSpacing];
    float bw      = (float)a->v[kParamBandwidth];

    float fc0 = base;
    float fc1 = base + spacing;
    float fc2 = base + 2.0f * spacing;

    // Clamp each cutoff to the valid range
    const float maxFreq = 15000.0f;
    if ( fc0 > maxFreq ) fc0 = maxFreq;
    if ( fc1 > maxFreq ) fc1 = maxFreq;
    if ( fc2 > maxFreq ) fc2 = maxFreq;

    computeBP( a->bpB0[0], a->bpA1[0], a->bpA2[0], fc0, bw, sr );
    computeBP( a->bpB0[1], a->bpA1[1], a->bpA2[1], fc1, bw, sr );
    computeBP( a->bpB0[2], a->bpA1[2], a->bpA2[2], fc2, bw, sr );
}

// ----------------------------------------------------------------------------
// parameterChanged
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
        a->satBias = a->v[kParamBias] / 100.0f;     // raw ±50 → normalised ±0.5
        break;
    case kParamDryWet:
        a->satDryWet = a->v[kParamDryWet] / 100.0f;
        break;
    case kParamBaseFreq:
    case kParamSpacing:
    case kParamBandwidth:
        updateBPFilters( a );
        break;
    case kParamFeedback:
        a->satFeedback = a->v[kParamFeedback] / 100.0f;
        break;
    }
}

// ----------------------------------------------------------------------------
// step
// ----------------------------------------------------------------------------

void step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
    BKICK* a = (BKICK*)self;
    int numFrames = numFramesBy4 * 4;

    const float* trig = busFrames + (a->v[kParamTrigger] - 1) * numFrames;
    float*       out  = busFrames + (a->v[kParamOutput]  - 1) * numFrames;

    // Copy state to locals
    float    phase          = a->phase;
    float    envPos         = a->envPos;
    float    prevTrig       = a->prevTrig;
    EnvState envState       = a->envState;
    float    pitchEnvPos    = a->pitchEnvPos;
    uint8_t  pitchEnvActive = a->pitchEnvActive;
    float    prevBpOut      = a->prevBpOut;

    // Biquad state locals
    float x1_0 = a->bpX1[0], x2_0 = a->bpX2[0], y1_0 = a->bpY1[0], y2_0 = a->bpY2[0];
    float x1_1 = a->bpX1[1], x2_1 = a->bpX2[1], y1_1 = a->bpY1[1], y2_1 = a->bpY2[1];
    float x1_2 = a->bpX1[2], x2_2 = a->bpX2[2], y1_2 = a->bpY1[2], y2_2 = a->bpY2[2];

    // Biquad coefficients (constant per block)
    const float b0_0 = a->bpB0[0], a1_0 = a->bpA1[0], a2_0 = a->bpA2[0];
    const float b0_1 = a->bpB0[1], a1_1 = a->bpA1[1], a2_1 = a->bpA2[1];
    const float b0_2 = a->bpB0[2], a1_2 = a->bpA1[2], a2_2 = a->bpA2[2];

    const float freqInc        = a->freqInc;
    const float levelV         = a->levelV;
    const float attackSamples  = a->attackSamples;
    const float attackExponent = a->attackExponent;
    const float holdSamples    = a->holdSamples;
    const float releaseSamples = a->releaseSamples;
    const float releaseExponent= a->releaseExponent;
    const float pitchAmount    = a->pitchAmount;
    const float pitchDecaySamp = a->pitchDecaySamples;
    const float pitchDecayExp  = a->pitchDecayExponent;
    const uint8_t pitchSlope   = a->pitchDecaySlope;
    const float satDrive       = a->satDrive;
    const float satBias        = a->satBias;
    const float satFeedback    = a->satFeedback;

    // Precompute saturation constants (constant per block)
    const bool  doSat     = (satDrive >= 0.001f);
    const float tanhDrive = doSat ? tanhf( satDrive ) : 1.0f;
    const float biasComp  = doSat ? tanhf( satDrive * satBias ) / tanhDrive : 0.0f;

    // Equal power dry/wet (constant per block)
    const float dryWet   = a->satDryWet;
    const float wetCoeff = sinf( dryWet * kPi * 0.5f );
    const float dryCoeff = cosf( dryWet * kPi * 0.5f );

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

        // Pitch envelope
        float pitchSemitones = 0.0f;
        if ( pitchEnvActive )
        {
            float n = pitchEnvPos / pitchDecaySamp;
            float c = powf( n, pitchDecayExp );
            pitchSemitones = ( pitchSlope == 0 )
                ? pitchAmount * (1.0f - c)
                : pitchAmount * (1.0f + cosf( kPi * c )) * 0.5f;
            pitchEnvPos += 1.0f;
            if ( pitchEnvPos >= pitchDecaySamp ) { pitchEnvPos = 0.0f; pitchEnvActive = 0; }
        }

        // Amp envelope
        float env = 0.0f;
        switch ( envState )
        {
        case kEnvAttack:
        {
            float c = powf( envPos / attackSamples, attackExponent );
            env = (1.0f - cosf( kPi * c )) * 0.5f;
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
            float c = powf( envPos / releaseSamples, releaseExponent );
            env = (1.0f + cosf( kPi * c )) * 0.5f;
            envPos += 1.0f;
            if ( envPos >= releaseSamples ) { envPos = 0.0f; envState = kEnvIdle; }
            break;
        }
        default:
            env = 0.0f;
            break;
        }

        // VCO
        float sineVal    = sinf( kTwoPi * phase );
        float vcaOut     = sineVal * levelV * env;  // post-VCA signal (volts)

        // Saturator input: post-VCA signal + feedback from BP output
        float satInput   = vcaOut + satFeedback * prevBpOut;

        // Tanh saturation (normalised to kSatScale, DC-compensated bias)
        float satOut;
        if ( !doSat )
        {
            satOut = satInput;
        }
        else
        {
            float norm   = satInput / kSatScale;
            float biased = norm + satBias;
            satOut = ( tanhf( satDrive * biased ) / tanhDrive - biasComp ) * kSatScale;
        }

        // Triple bandpass filter (3 parallel biquads, averaged)
        // Direct Form II Transposed: y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2
        // b1=0, b2=-b0 for bandpass
        float bp0 = b0_0 * satOut - b0_0 * x2_0 - a1_0 * y1_0 - a2_0 * y2_0;
        x2_0 = x1_0; x1_0 = satOut; y2_0 = y1_0; y1_0 = bp0;

        float bp1 = b0_1 * satOut - b0_1 * x2_1 - a1_1 * y1_1 - a2_1 * y2_1;
        x2_1 = x1_1; x1_1 = satOut; y2_1 = y1_1; y1_1 = bp1;

        float bp2 = b0_2 * satOut - b0_2 * x2_2 - a1_2 * y1_2 - a2_2 * y2_2;
        x2_2 = x1_2; x1_2 = satOut; y2_2 = y1_2; y1_2 = bp2;

        float bpOut  = (bp0 + bp1 + bp2) / 3.0f;
        prevBpOut    = bpOut;

        // Equal power dry/wet: dry=vcaOut (pre-saturator), wet=bpOut (post-filter)
        out[i] = dryCoeff * vcaOut + wetCoeff * bpOut;

        // Advance VCO phase
        phase += freqInc * exp2f( pitchSemitones / 12.0f );
        if ( phase >= 1.0f ) phase -= 1.0f;
    }

    // Write state back
    a->phase          = phase;
    a->envPos         = envPos;
    a->prevTrig       = prevTrig;
    a->envState       = envState;
    a->pitchEnvPos    = pitchEnvPos;
    a->pitchEnvActive = pitchEnvActive;
    a->prevBpOut      = prevBpOut;

    a->bpX1[0]=x1_0; a->bpX2[0]=x2_0; a->bpY1[0]=y1_0; a->bpY2[0]=y2_0;
    a->bpX1[1]=x1_1; a->bpX2[1]=x2_1; a->bpY1[1]=y1_1; a->bpY2[1]=y2_1;
    a->bpX1[2]=x1_2; a->bpX2[2]=x2_2; a->bpY1[2]=y1_2; a->bpY2[2]=y2_2;
}

// ----------------------------------------------------------------------------
// Factory and entry point
// ----------------------------------------------------------------------------

static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('B','K','C','K'),
    .name                        = "BKICK",
    .description                 = "Bass kick: sine VCO, AHR amp/pitch envelopes, tanh saturator, triple bandpass",
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
