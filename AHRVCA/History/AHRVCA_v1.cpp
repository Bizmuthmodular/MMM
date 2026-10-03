#include "distingnt/api.h"
#include <cmath>
#include <cstdint>
#include <new>

// ---------------------------------------------------------------------------
// AHRVCA — Attack/Hold/Release envelope generator driving two linear VCAs.
//
// Envelope state machine (state flow, trigger detection, ms->samples timing)
// is the same structure SNARR uses for its body/noise amplitude envelopes.
// The per-segment shape is a variable power curve (AttackCurve/ReleaseCurve)
// instead of SNARR's fixed one-pole recursion, so the curve can be swept
// from a straight ramp (0%) to a strongly curved, exponential-like shape
// (100%).
// ---------------------------------------------------------------------------

static const char* const kOffOnNames[] = { "OFF", "ON", NULL };

// Maximum power-curve exponent reached at Curve = 100%.
static const float kMaxCurveExponent = 6.0f;

// ---------------------------------------------------------------------------
// Envelope state constants — same layout SNARR uses.
// ---------------------------------------------------------------------------
enum { kEnvIdle = 0, kEnvAttack = 1, kEnvHold = 2, kEnvRelease = 3 };

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
enum {
    // Envelope page
    kParamAttack,           //  0
    kParamHold,             //  1
    kParamRelease,          //  2
    kParamAttackCurve,      //  3
    kParamReleaseCurve,     //  4
    // VCA page
    kParamOffset,           //  5
    kParamEnvScale,         //  6
    kParamOutputLevel,      //  7
    kParamMute,             //  8
    // Routing page
    kParamTriggerIn,        //  9
    kParamAudioIn1,         // 10
    kParamAudioIn2,         // 11
    kParamOut1,             // 12
    kParamOut1Mode,         // 13
    kParamOut2,             // 14
    kParamOut2Mode,         // 15
    kParamSlopeOutPos,      // 16
    kParamSlopeOutPosMode,  // 17
    kParamSlopeOutNeg,      // 18
    kParamSlopeOutNegMode,  // 19
    kNumParams              // 20
};

static const _NT_parameter parameters[kNumParams] = {
    // Envelope page (0-4)
    { .name="Attack",        .min=1,    .max=500, .def=5,   .unit=kNT_unitMs,      .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Hold",          .min=1,    .max=500, .def=10,  .unit=kNT_unitMs,      .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Release",       .min=1,    .max=500, .def=150, .unit=kNT_unitMs,      .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="AttackCurve",   .min=0,    .max=100, .def=100, .unit=kNT_unitPercent, .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="ReleaseCurve",  .min=0,    .max=100, .def=100, .unit=kNT_unitPercent, .scaling=kNT_scalingNone, .enumStrings=NULL },
    // VCA page (5-8)
    { .name="Offset",        .min=-100, .max=100, .def=0,   .unit=kNT_unitVolts,   .scaling=kNT_scaling10,   .enumStrings=NULL },
    { .name="EnvScale",      .min=-100, .max=100, .def=100, .unit=kNT_unitPercent, .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Output Level",  .min=0,    .max=100, .def=100, .unit=kNT_unitPercent, .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Mute",          .min=0,    .max=1,   .def=0,   .unit=kNT_unitEnum,    .scaling=kNT_scalingNone, .enumStrings=kOffOnNames },
    // Routing page (9-19)
    NT_PARAMETER_CV_INPUT(              "TriggerIn", 1, 1  )
    NT_PARAMETER_AUDIO_INPUT(           "AudioIn_1", 1, 2  )
    NT_PARAMETER_AUDIO_INPUT(           "AudioIn_2", 0, 0  )
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Out1",      1, 13 )
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Out2",      0, 14 )
    NT_PARAMETER_CV_OUTPUT_WITH_MODE(   "+SlopeOut", 0, 0  )
    NT_PARAMETER_CV_OUTPUT_WITH_MODE(   "-SlopeOut", 0, 0  )
};

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
static const uint8_t envelopeParams[] = { kParamAttack, kParamHold, kParamRelease,
                                           kParamAttackCurve, kParamReleaseCurve };
static const uint8_t vcaParams[]      = { kParamOffset, kParamEnvScale,
                                           kParamOutputLevel, kParamMute };
static const uint8_t routingParams[]  = { kParamTriggerIn, kParamAudioIn1, kParamAudioIn2,
                                           kParamOut1, kParamOut1Mode, kParamOut2, kParamOut2Mode,
                                           kParamSlopeOutPos, kParamSlopeOutPosMode,
                                           kParamSlopeOutNeg, kParamSlopeOutNegMode };

static const _NT_parameterPage pages[] = {
    { .name="Envelope", .numParams=5,  .group=0, .params=envelopeParams },
    { .name="VCA",      .numParams=4,  .group=0, .params=vcaParams      },
    { .name="Routing",  .numParams=11, .group=0, .params=routingParams  },
};

static const _NT_parameterPages paramPages = { .numPages=3, .pages=pages };

// ---------------------------------------------------------------------------
// Algorithm structs
// ---------------------------------------------------------------------------
struct AhrVcaAlg_DTC {
    float envCounter;
    int   envState;
    float envOut;            // current envelope value, 0..1, post curve shaping
    float prevTrig;          // trigger edge detection
    float outLevelSmoothed;  // smoothed Output Level / Mute gain, 0..1
};

struct AhrVcaAlg : public _NT_algorithm {
    AhrVcaAlg_DTC* dtc;

    // Envelope timing, cached from ms parameters
    float attackSamples;
    float holdSamples;
    float releaseSamples;
    float attackCurveExp;
    float releaseCurveExp;

    // VCA cached values
    float offsetVolts;
    float envScaleFrac;
    float outLevelFrac;
    bool  mute;
    float outLevelSmoothCoeff;

    AhrVcaAlg(AhrVcaAlg_DTC* d) : dtc(d),
        attackSamples(0.0f), holdSamples(0.0f), releaseSamples(0.0f),
        attackCurveExp(1.0f), releaseCurveExp(1.0f),
        offsetVolts(0.0f), envScaleFrac(0.0f), outLevelFrac(0.0f),
        mute(false), outLevelSmoothCoeff(1.0f)
    {}
};

// ---------------------------------------------------------------------------
// Coefficient helpers
// ---------------------------------------------------------------------------
// Same one-pole attack-coefficient formula SNARR uses (reused here only for
// the fixed-time Output Level smoother).
static inline float msAttackCoeff(float ms, float sr) {
    const float samps = ms * 0.001f * sr;
    return (samps > 1.0f) ? (1.0f - expf(-1.0f / samps)) : 1.0f;
}

// Maps a 0..100 Curve parameter to a power-curve exponent: 100 (0%) -> 1.0
// (linear), and 1.0 (100%) -> kMaxCurveExponent (strongly curved).
static inline float curveExponent(int16_t raw) {
    const float frac = raw * 0.01f;
    return 1.0f + frac * (kMaxCurveExponent - 1.0f);
}

// ---------------------------------------------------------------------------
// calculateRequirements
// ---------------------------------------------------------------------------
void calculateRequirements(_NT_algorithmRequirements& req, const int32_t*) {
    req.numParameters = kNumParams;
    req.sram = sizeof(AhrVcaAlg);
    req.dtc  = sizeof(AhrVcaAlg_DTC);
    req.dram = 0;
    req.itc  = 0;
}

// ---------------------------------------------------------------------------
// construct
// ---------------------------------------------------------------------------
_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs,
                         const _NT_algorithmRequirements&,
                         const int32_t*)
{
    AhrVcaAlg_DTC* d = (AhrVcaAlg_DTC*)ptrs.dtc;
    d->envCounter       = 0.0f;
    d->envState         = kEnvIdle;
    d->envOut           = 0.0f;
    d->prevTrig         = 0.0f;
    d->outLevelSmoothed = 0.0f;

    AhrVcaAlg* a = new (ptrs.sram) AhrVcaAlg(d);
    a->parameters     = parameters;
    a->parameterPages = &paramPages;

    const float sr = NT_globals.sampleRate;
    a->attackSamples  = a->v[kParamAttack]  * 0.001f * sr;
    a->holdSamples    = a->v[kParamHold]    * 0.001f * sr;
    a->releaseSamples = a->v[kParamRelease] * 0.001f * sr;
    a->attackCurveExp  = curveExponent(a->v[kParamAttackCurve]);
    a->releaseCurveExp = curveExponent(a->v[kParamReleaseCurve]);

    a->offsetVolts    = a->v[kParamOffset]      / 10.0f;
    a->envScaleFrac   = a->v[kParamEnvScale]    / 100.0f;
    a->outLevelFrac   = a->v[kParamOutputLevel] / 100.0f;
    a->mute           = a->v[kParamMute] != 0;
    a->outLevelSmoothCoeff = msAttackCoeff(20.0f, sr);  // fixed 20 ms smoothing

    return a;
}

// ---------------------------------------------------------------------------
// parameterChanged
// ---------------------------------------------------------------------------
void parameterChanged(_NT_algorithm* self, int p) {
    AhrVcaAlg* a = (AhrVcaAlg*)self;
    const float sr = NT_globals.sampleRate;
    switch (p) {
    case kParamAttack:
        a->attackSamples = a->v[kParamAttack] * 0.001f * sr;
        break;
    case kParamHold:
        a->holdSamples = a->v[kParamHold] * 0.001f * sr;
        break;
    case kParamRelease:
        a->releaseSamples = a->v[kParamRelease] * 0.001f * sr;
        break;
    case kParamAttackCurve:
        a->attackCurveExp = curveExponent(a->v[kParamAttackCurve]);
        break;
    case kParamReleaseCurve:
        a->releaseCurveExp = curveExponent(a->v[kParamReleaseCurve]);
        break;
    case kParamOffset:
        a->offsetVolts = a->v[kParamOffset] / 10.0f;
        break;
    case kParamEnvScale:
        a->envScaleFrac = a->v[kParamEnvScale] / 100.0f;
        break;
    case kParamOutputLevel:
        a->outLevelFrac = a->v[kParamOutputLevel] / 100.0f;
        break;
    case kParamMute:
        a->mute = a->v[kParamMute] != 0;
        break;
    }
}

// ---------------------------------------------------------------------------
// step
// ---------------------------------------------------------------------------
void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    AhrVcaAlg*     a = (AhrVcaAlg*)self;
    AhrVcaAlg_DTC* d = a->dtc;
    const int n = numFramesBy4 * 4;

    const int    trigBus = a->v[kParamTriggerIn];
    const float* trig    = (trigBus > 0) ? busFrames + (trigBus - 1) * n : nullptr;

    const int    audio1Bus = a->v[kParamAudioIn1];
    const float* audio1    = (audio1Bus > 0) ? busFrames + (audio1Bus - 1) * n : nullptr;
    const int    audio2Bus = a->v[kParamAudioIn2];
    const float* audio2    = (audio2Bus > 0) ? busFrames + (audio2Bus - 1) * n : nullptr;
    const bool   vca2Active = (audio2 != nullptr);

    const int   out1Bus     = a->v[kParamOut1];
    float*      out1        = (out1Bus > 0) ? busFrames + (out1Bus - 1) * n : nullptr;
    const bool  out1Replace = a->v[kParamOut1Mode] != 0;
    const int   out2Bus     = a->v[kParamOut2];
    float*      out2        = (out2Bus > 0) ? busFrames + (out2Bus - 1) * n : nullptr;
    const bool  out2Replace = a->v[kParamOut2Mode] != 0;

    const int   slopePosBus     = a->v[kParamSlopeOutPos];
    float*      slopePos        = (slopePosBus > 0) ? busFrames + (slopePosBus - 1) * n : nullptr;
    const bool  slopePosReplace = a->v[kParamSlopeOutPosMode] != 0;
    const int   slopeNegBus     = a->v[kParamSlopeOutNeg];
    float*      slopeNeg        = (slopeNegBus > 0) ? busFrames + (slopeNegBus - 1) * n : nullptr;
    const bool  slopeNegReplace = a->v[kParamSlopeOutNegMode] != 0;

    // Pull DTC into locals
    float envCounter = d->envCounter;
    int   envState   = d->envState;
    float envOut     = d->envOut;
    float prevTrig   = d->prevTrig;
    float outLevelSm = d->outLevelSmoothed;

    // Cache algo params
    const float attackSamples  = a->attackSamples;
    const float holdSamples    = a->holdSamples;
    const float releaseSamples = a->releaseSamples;
    const float attackK        = a->attackCurveExp;
    const float releaseK       = a->releaseCurveExp;
    const float offsetVolts    = a->offsetVolts;
    const float envScaleFrac   = a->envScaleFrac;
    const float outLevelTarget = a->mute ? 0.0f : a->outLevelFrac;
    const float smoothCoeff    = a->outLevelSmoothCoeff;

    for (int i = 0; i < n; ++i) {

        // ---- Trigger detection (rising edge at 0.5 V threshold) ----
        if (trig) {
            const float t = trig[i];
            if (prevTrig < 0.5f && t >= 0.5f) {
                envState   = kEnvAttack;
                envCounter = 0.0f;
            }
            prevTrig = t;
        }

        // ---- AHR envelope, power-curve shaped ----
        switch (envState) {
        case kEnvAttack: {
            float t = (attackSamples > 0.0f) ? (envCounter / attackSamples) : 1.0f;
            if (t >= 1.0f) { t = 1.0f; envState = kEnvHold; envCounter = 0.0f; }
            else envCounter += 1.0f;
            envOut = 1.0f - powf(1.0f - t, attackK);
            break;
        }
        case kEnvHold:
            envOut = 1.0f;
            envCounter += 1.0f;
            if (envCounter >= holdSamples) { envState = kEnvRelease; envCounter = 0.0f; }
            break;
        case kEnvRelease: {
            float t = (releaseSamples > 0.0f) ? (envCounter / releaseSamples) : 1.0f;
            if (t >= 1.0f) { t = 1.0f; envState = kEnvIdle; envCounter = 0.0f; }
            else envCounter += 1.0f;
            envOut = powf(1.0f - t, releaseK);
            break;
        }
        default:
            envOut = 0.0f;
            break;
        }

        // ---- Slope CV: envelope (0..+10V) scaled and offset, clipped to +-10V ----
        float slopeVolts = offsetVolts + envScaleFrac * (envOut * 10.0f);
        if (slopeVolts >  10.0f) slopeVolts =  10.0f;
        if (slopeVolts < -10.0f) slopeVolts = -10.0f;

        // ---- Linear VCA law: 0V -> gain 0, +10V -> gain 1 ----
        float gain = slopeVolts * 0.1f;
        if (gain < 0.0f) gain = 0.0f;
        if (gain > 1.0f) gain = 1.0f;

        // ---- Output Level / Mute smoothing (avoids zipper noise) ----
        outLevelSm += (outLevelTarget - outLevelSm) * smoothCoeff;
        const float finalGain = gain * outLevelSm;

        // ---- VCA 1 ----
        if (out1) {
            const float s = audio1 ? audio1[i] * finalGain : 0.0f;
            if (out1Replace) out1[i] = s; else out1[i] += s;
        }
        // ---- VCA 2 (only active when AudioIn_2 is connected) ----
        if (vca2Active && out2) {
            const float s = audio2[i] * finalGain;
            if (out2Replace) out2[i] = s; else out2[i] += s;
        }

        // ---- Envelope slope outputs, normal and inverted ----
        if (slopePos) {
            if (slopePosReplace) slopePos[i] = slopeVolts; else slopePos[i] += slopeVolts;
        }
        if (slopeNeg) {
            if (slopeNegReplace) slopeNeg[i] = -slopeVolts; else slopeNeg[i] += -slopeVolts;
        }
    }

    // Write locals back to DTC
    d->envCounter       = envCounter;
    d->envState         = envState;
    d->envOut           = envOut;
    d->prevTrig         = prevTrig;
    d->outLevelSmoothed = outLevelSm;
}

// ---------------------------------------------------------------------------
// Factory and entry point
// ---------------------------------------------------------------------------
static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('A','H','R','V'),
    .name                        = "AHRVCA",
    .description                 = "AHR envelope generator with dual linear VCA",
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

extern "C" uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
    switch (selector) {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return (uintptr_t)(data == 0 ? &factory : NULL);
    }
    return 0;
}
