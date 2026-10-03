#include "distingnt/api.h"
#include "distingnt/wav.h"
#include <cmath>
#include <cstring>
#include <new>

// ─── PLACEHOLDER PLUGIN ─────────────────────────────────────────────────────
// This is scaffolding only. The parameter/page tables and memory wiring below
// are final (per "Wavetable Envelope Plan.md" in this folder); the actual
// per-sample envelope/wavetable-scan DSP in step() has NOT been written yet.
// See "Wavetable Envelope Plan.md" for the full design, formulas and
// near-final code to transcribe into step()/parameterChanged().
// ─────────────────────────────────────────────────────────────────────────────

enum EnvState : uint8_t { kEnvIdle = 0, kEnvRise = 1, kEnvFall = 2 };

// ─── DTC — true per-sample running state (see plan doc for field meanings) ──
struct WavetableSlopeAlg_DTC {
    EnvState envState;
    uint32_t envSampleCount;
    float    tablePhase;
    float    prevTrigIn;
    // Snapshot-at-trigger (frozen for the life of the current envelope):
    const int16_t* tableBaseSnap;
    uint32_t windowStartSnap, windowSizeSnap;
    uint32_t riseSamplesSnap, fallSamplesSnap;
    float    riseRateBaseSnap, fallRateBaseSnap, stretchFactorSnap;
};

// ─── Algorithm struct ────────────────────────────────────────────────────────
struct WavetableSlopeAlg : public _NT_algorithm {
    WavetableSlopeAlg_DTC* dtc;

    _NT_parameter        params[9];   // mutable copy of parameters[] — .max patched at SD-mount time
    _NT_wavetableRequest request;     // persistent — must not be a stack object

    bool cardMounted;
    bool awaitingCallback;
    bool tableReady;

    explicit WavetableSlopeAlg( WavetableSlopeAlg_DTC* d )
        : dtc(d), cardMounted(false), awaitingCallback(false), tableReady(false)
    {}
};

// ─── Parameters ──────────────────────────────────────────────────────────────
enum {
    kParamWavetable,
    kParamWindow,
    kParamOffset,
    kParamRiseTime,
    kParamFallTime,
    kParamStretch,
    kParamTriggerIn,
    kParamSlopeOut,
    kParamSlopeOutMode,
    kNumParams
};

static const _NT_parameter parameters[] = {
    { .name="Wavetable", .min=0,  .max=32767, .def=0,   .unit=kNT_unitHasStrings, .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Window",    .min=2,  .max=1000,  .def=100, .unit=kNT_unitFrames,     .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Offset",    .min=0,  .max=1000,  .def=0,   .unit=kNT_unitPercent,    .scaling=kNT_scaling10,   .enumStrings=NULL },
    { .name="RiseTime",  .min=1,  .max=1000,  .def=100, .unit=kNT_unitSeconds,    .scaling=kNT_scaling100,  .enumStrings=NULL },
    { .name="FallTime",  .min=1,  .max=1000,  .def=100, .unit=kNT_unitSeconds,    .scaling=kNT_scaling100,  .enumStrings=NULL },
    { .name="Stretch",   .min=10, .max=1000,  .def=100, .unit=kNT_unitPercent,    .scaling=kNT_scalingNone, .enumStrings=NULL },
    NT_PARAMETER_CV_INPUT( "TriggerIn", 0, 0 )
    NT_PARAMETER_CV_OUTPUT_WITH_MODE( "SlopeOut", 1, 13 )
};

// ─── Pages ───────────────────────────────────────────────────────────────────
static const uint8_t slopePageParams[]   = { kParamWavetable, kParamWindow, kParamOffset,
                                              kParamRiseTime, kParamFallTime, kParamStretch };
static const uint8_t routingPageParams[] = { kParamTriggerIn, kParamSlopeOut, kParamSlopeOutMode };

static const _NT_parameterPage pages[] = {
    { .name="Slope",   .numParams=ARRAY_SIZE(slopePageParams),   .group=0, .params=slopePageParams },
    { .name="Routing", .numParams=ARRAY_SIZE(routingPageParams), .group=0, .params=routingPageParams },
};
static const _NT_parameterPages parameterPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages    = pages,
};

// ─── Wavetable buffer sizing ──────────────────────────────────────────────────
// Generous fixed DRAM allocation (must hold the whole loaded wavetable, mip
// pyramid included, since Offset navigates the entire file). See plan doc —
// this constant is a best guess pending real /wavetables/ file testing.
static const uint32_t kWavetableBufferFrames = 256u * 2048u * 2u; // ~2MB int16

// ─── Static memory ────────────────────────────────────────────────────────────
void calculateRequirements( _NT_algorithmRequirements& req, const int32_t* )
{
    req.numParameters = ARRAY_SIZE(parameters);
    req.sram = sizeof(WavetableSlopeAlg);
    req.dram = kWavetableBufferFrames * sizeof(int16_t);
    req.dtc  = sizeof(WavetableSlopeAlg_DTC);
    req.itc  = 0;
}

// ─── Async wavetable load callback ────────────────────────────────────────────
static void requestCallback( void* callbackData )
{
    WavetableSlopeAlg* a = (WavetableSlopeAlg*)callbackData;
    a->awaitingCallback = false;
    a->tableReady = !a->request.error && a->request.numWaves > 0 && a->request.waveLength > 0;
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────
_NT_algorithm* construct( const _NT_algorithmMemoryPtrs& ptrs,
                           const _NT_algorithmRequirements& req,
                           const int32_t* )
{
    memset( ptrs.dtc, 0, sizeof(WavetableSlopeAlg_DTC) );

    WavetableSlopeAlg* a = new (ptrs.sram) WavetableSlopeAlg( (WavetableSlopeAlg_DTC*)ptrs.dtc );

    a->request.table     = (int16_t*)ptrs.dram;
    a->request.tableSize = req.dram / sizeof(int16_t);
    memset( a->request.table, 0, req.dram );
    a->request.callback     = requestCallback;
    a->request.callbackData = a;

    memcpy( a->params, parameters, sizeof(parameters) );
    a->parameters     = a->params;      // mutable copy — .max patched at SD-mount time
    a->parameterPages = &parameterPages;

    return a;
}

void parameterChanged( _NT_algorithm* self, int p )
{
    WavetableSlopeAlg* a = (WavetableSlopeAlg*)self;
    switch ( p )
    {
    case kParamWavetable:
        if ( !a->awaitingCallback )
        {
            a->request.index = a->v[kParamWavetable];
            a->tableReady = false;
            if ( NT_readWavetable( a->request ) )
                a->awaitingCallback = true;
        }
        break;
    // TODO: no other parameters need caching yet — see plan doc. Every
    // DSP-relevant parameter (Window/Offset/RiseTime/FallTime/Stretch) is
    // only converted to sample-domain values at the instant of a trigger,
    // inside step(), once that's implemented.
    }
}

int parameterString( _NT_algorithm*, int p, int v, char* buff )
{
    int len = 0;
    if ( p == kParamWavetable )
    {
        _NT_wavetableInfo info;
        NT_getWavetableInfo( v, info );
        if ( info.name )
        {
            strncpy( buff, info.name, kNT_parameterStringSize - 1 );
            buff[ kNT_parameterStringSize - 1 ] = 0;
            len = (int)strlen( buff );
        }
    }
    return len;
}

// ─── Audio processing — NOT YET IMPLEMENTED ──────────────────────────────────
// See "Wavetable Envelope Plan.md" in this folder for the full step() design:
// trigger detection on TriggerIn, snapshot-at-trigger of Window/Offset/
// RiseTime/FallTime/Stretch, linear envelope × interpolated wavetable-window
// read, output scaling/clamp to SlopeOut. Currently a no-op — leaves all
// buses untouched, and also handles the SD-card-mount / dynamic Wavetable
// range patch that step() is responsible for polling.
void step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
    WavetableSlopeAlg* a = (WavetableSlopeAlg*)self;
    (void)busFrames;
    (void)numFramesBy4;

    // SD card mount handling (real — needed even before the DSP is written,
    // so Wavetable selection works once step() starts being called).
    bool cardMounted = NT_isSdCardMounted();
    if ( a->cardMounted != cardMounted )
    {
        a->cardMounted = cardMounted;
        if ( cardMounted )
        {
            a->params[kParamWavetable].max = NT_getNumWavetables() - 1;
            NT_updateParameterDefinition( NT_algorithmIndex(self), kParamWavetable );
            parameterChanged( self, kParamWavetable );
        }
    }

    // TODO: envelope + wavetable-window DSP goes here. See plan doc.
}

// ─── Factory ──────────────────────────────────────────────────────────────────
static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('W','t','S','l'),
    .name                        = "WavetableSlope",
    .description                 = "Wavetable-window scanned, linear rise/fall CV slope generator",
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
    .parameterString             = parameterString,
};

extern "C" uintptr_t pluginEntry( _NT_selector selector, uint32_t data )
{
    switch ( selector ) {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return (uintptr_t)(data == 0 ? &factory : NULL);
    }
    return 0;
}
