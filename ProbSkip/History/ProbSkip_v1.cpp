#include "distingnt/api.h"
#include <new>

// ----------------------------------------------------------------------------
// LCG random generator — returns a pseudo-random float in [0, 100)
// ----------------------------------------------------------------------------

static float lcgNext0to100( uint32_t& s )
{
    s = s * 1664525u + 1013904223u;
    return (float)(s >> 8) * (100.0f / 16777216.0f);   // top 24 bits -> [0,100)
}

// ----------------------------------------------------------------------------
// Algorithm struct
// ----------------------------------------------------------------------------

struct ProbSkip : public _NT_algorithm
{
    float    probability;   // cached from v[kParamProbability], 0..100
    float    prevIn;        // previous sample value, for rising-edge detection
    bool     passing;       // whether the currently-open gate is being passed through
    uint32_t rngState;      // LCG state, seeded per instance in construct()

    explicit ProbSkip( uint32_t seed )
        : probability ( 100.0f )
        , prevIn      ( 0.0f )
        , passing     ( false )
        , rngState    ( seed )
    {
    }
};

// ----------------------------------------------------------------------------
// Parameters
// ----------------------------------------------------------------------------

enum { kParamProbability, kParamInput, kParamOutput };

static const _NT_parameter parameters[] = {
    { .name="Probability", .min=0, .max=100, .def=100, .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    NT_PARAMETER_CV_INPUT( "Input", 1, 1 )
    NT_PARAMETER_CV_OUTPUT( "Output", 1, 13 )
};

static const uint8_t pageSkipper[] = { kParamProbability };
static const uint8_t pageRouting[] = { kParamInput, kParamOutput };

static const _NT_parameterPage pages[] = {
    { .name="Skipper", .numParams=1, .group=0, .params=pageSkipper },
    { .name="Routing", .numParams=2, .group=0, .params=pageRouting },
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
    req.sram = sizeof(ProbSkip);
    req.dram = 0;
    req.dtc  = 0;
    req.itc  = 0;
}

_NT_algorithm* construct( const _NT_algorithmMemoryPtrs& ptrs,
                           const _NT_algorithmRequirements&,
                           const int32_t* )
{
    // Seed the per-instance RNG from a free-running counter mixed with the
    // instance's own SRAM address, so multiple instances in one preset don't
    // draw identical random sequences.
    static uint32_t sConstructSeed = 0xA53F921Du;
    sConstructSeed = sConstructSeed * 1664525u + 1013904223u;
    uint32_t seed = sConstructSeed ^ (uint32_t)(uintptr_t)ptrs.sram;

    ProbSkip* a = new (ptrs.sram) ProbSkip( seed );
    a->parameters     = parameters;
    a->parameterPages = &paramPages;
    return a;
}

void parameterChanged( _NT_algorithm* self, int p )
{
    ProbSkip* a = (ProbSkip*)self;
    if ( p == kParamProbability )
        a->probability = (float)a->v[kParamProbability];
}

// ----------------------------------------------------------------------------
// step
// ----------------------------------------------------------------------------

void step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
    ProbSkip* a = (ProbSkip*)self;
    int numFrames = numFramesBy4 * 4;

    const float* in  = busFrames + (a->v[kParamInput]  - 1) * numFrames;
    float*       out = busFrames + (a->v[kParamOutput] - 1) * numFrames;

    float          prevIn      = a->prevIn;
    bool           passing     = a->passing;
    uint32_t       rngState    = a->rngState;
    const float    probability = a->probability;

    for ( int i = 0; i < numFrames; ++i )
    {
        float x = in[i];

        if ( prevIn < 1.0f && x >= 1.0f )
        {
            // Rising edge: roll the dice once for the whole gate/trigger.
            float roll = lcgNext0to100( rngState );
            passing = ( roll < probability );
        }

        out[i] = ( x >= 1.0f && passing ) ? x : 0.0f;

        prevIn = x;
    }

    a->prevIn   = prevIn;
    a->passing  = passing;
    a->rngState = rngState;
}

// ----------------------------------------------------------------------------
// Factory
// ----------------------------------------------------------------------------

static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('P','S','k','p'),
    .name                        = "ProbSkip",
    .description                 = "Probability-based trigger/gate skipper",
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
    .tags                        = kNT_tagUtility,
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
