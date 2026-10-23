#include "distingnt/api.h"
#include <cmath>
#include <new>

// ----------------------------------------------------------------------------
// 4x4 Matrix Feedback Generator — scaffold (Implementation Plan Step 1)
//
// This is a minimal, compilable, loadable disting NT algorithm. It exists to
// validate the build pipeline before any real DSP is added. step() outputs
// silence. No parameters from 4x4-System-Overview.md yet — those arrive in
// later implementation steps.
// ----------------------------------------------------------------------------

struct FourByFour : public _NT_algorithm
{
    FourByFour()
    {
        parameters       = nullptr;
        parameterPages   = nullptr;
        vIncludingCommon  = nullptr;
        v                 = nullptr;
    }
};

// ----------------------------------------------------------------------------
// Parameters (placeholder — a single output bus so the scaffold has
// somewhere to write silence to and can be loaded/inspected on the module)
// ----------------------------------------------------------------------------

enum { kParamOutput, kParamOutputMode };

static const _NT_parameter parameters[] = {
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output", 1, 13 )
};

static const uint8_t page1[] = { kParamOutput, kParamOutputMode };

static const _NT_parameterPage pages[] = {
    { .name = "Routing", .numParams = ARRAY_SIZE( page1 ), .group = 0, .params = page1 },
};

static const _NT_parameterPages parameterPages = {
    .numPages = ARRAY_SIZE( pages ),
    .pages    = pages,
};

// ----------------------------------------------------------------------------
// Factory callbacks
// ----------------------------------------------------------------------------

void calculateRequirements( _NT_algorithmRequirements& req, const int32_t* /*specifications*/ )
{
    req.numParameters = ARRAY_SIZE( parameters );
    req.sram = sizeof( FourByFour );
    req.dram = 0;
    req.dtc  = 0;
    req.itc  = 0;
}

_NT_algorithm* construct( const _NT_algorithmMemoryPtrs& ptrs,
                           const _NT_algorithmRequirements& /*req*/,
                           const int32_t* /*specifications*/ )
{
    FourByFour* alg = new ( ptrs.sram ) FourByFour();
    alg->parameters     = parameters;
    alg->parameterPages = &parameterPages;
    return alg;
}

void step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
    FourByFour* a = (FourByFour*)self;
    int n = numFramesBy4 * 4;

    bool  replace = a->v[kParamOutputMode];
    int   outBus  = a->v[kParamOutput];
    if ( outBus < 1 ) return;
    float* out = busFrames + ( outBus - 1 ) * n;

    if ( replace )
        for ( int i = 0; i < n; ++i ) out[i] = 0.0f;
    // Add mode: do nothing, zero contributed.
}

// ----------------------------------------------------------------------------
// Factory
// ----------------------------------------------------------------------------

static const _NT_factory factory = {
    .guid                         = NT_MULTICHAR( '4', 'x', '4', 'S' ),
    .name                         = "4x4 Scaffold",
    .description                  = "4x4 Matrix Feedback Generator (build scaffold, Step 1)",
    .numSpecifications            = 0,
    .specifications               = NULL,
    .calculateStaticRequirements  = NULL,
    .initialise                   = NULL,
    .calculateRequirements        = calculateRequirements,
    .construct                    = construct,
    .parameterChanged             = NULL,
    .step                         = step,
    .draw                         = NULL,
    .midiRealtime                 = NULL,
    .midiMessage                  = NULL,
    .tags                         = kNT_tagUtility,
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
