#include "distingnt/api.h"
#include <cmath>
#include <cstdint>
#include <new>

static const float kSnarrPi = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Envelope state constants
// ---------------------------------------------------------------------------
enum { kEnvIdle = 0, kEnvAttack = 1, kEnvHold = 2, kEnvRelease = 3 };

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
enum {
    // Body page
    kParamFreq,       //  0
    kParamBAttack,    //  1
    kParamBHold,      //  2
    kParamBRel,       //  3
    kParamBendDecay,  //  4
    kParamBendAmt,    //  5
    // Noise page
    kParamNAttack,    //  6
    kParamNHold,      //  7
    kParamNRel,       //  8
    kParamTilt,       //  9
    // Routing page
    kParamTrigIn,     // 10
    kParamOutput,     // 11
    kNumParams        // 12
};

static const _NT_parameter parameters[kNumParams] = {
    // Body page (0-5)
    { .name="Freq",      .min=50,  .max=2000, .def=200,
      .unit=kNT_unitHz,      .scaling=0, .enumStrings=NULL },
    { .name="BAttack",   .min=1,   .max=500, .def=5,
      .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    { .name="BHold",     .min=1,   .max=500, .def=10,
      .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    { .name="BRel",      .min=1,   .max=500, .def=150,
      .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    { .name="BendDecay", .min=1,   .max=500, .def=80,
      .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    { .name="BendAmt",   .min=0,   .max=100,  .def=50,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    // Noise page (6-9)
    { .name="NAttack",   .min=1,   .max=500, .def=1,
      .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    { .name="NHold",     .min=1,   .max=500, .def=5,
      .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    { .name="NRel",      .min=1,   .max=500, .def=200,
      .unit=kNT_unitMs,      .scaling=0, .enumStrings=NULL },
    { .name="Tilt",      .min=-50, .max=50,   .def=0,
      .unit=kNT_unitNone,    .scaling=0, .enumStrings=NULL },
    // Routing page (10-11)
    NT_PARAMETER_CV_INPUT( "TrigIn", 1, 1 )
    NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )
};

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
static const uint8_t bodyParams[]    = { kParamFreq, kParamBAttack, kParamBHold,
                                          kParamBRel, kParamBendDecay, kParamBendAmt };
static const uint8_t noiseParams[]   = { kParamNAttack, kParamNHold, kParamNRel, kParamTilt };
static const uint8_t routingParams[] = { kParamTrigIn, kParamOutput };

static const _NT_parameterPage pages[] = {
    { .name="Body",    .numParams=6, .group=0, .params=bodyParams    },
    { .name="Noise",   .numParams=4, .group=0, .params=noiseParams   },
    { .name="Routing", .numParams=2, .group=0, .params=routingParams },
};

static const _NT_parameterPages paramPages = { .numPages=3, .pages=pages };

// ---------------------------------------------------------------------------
// Algorithm structs
// ---------------------------------------------------------------------------
struct SnarrAlg_DTC {
    float    sinePhase;
    // Body amplitude envelope
    float    bodyEnv;
    int      bodyState;
    float    bodyCounter;
    // Bend envelope (instant-on, single exp decay)
    float    bendEnv;
    // Noise amplitude envelope
    float    noiseEnv;
    int      noiseState;
    float    noiseCounter;
    // Tilt EQ one-pole state
    float    noiseLP;
    // Noise LCG seed
    uint32_t noiseSeed;
    // Trigger edge detection
    float    prevTrig;
};

struct SnarrAlg : public _NT_algorithm {
    SnarrAlg_DTC* dtc;
    // Cached derived values
    float freqHz;
    float bodyAttackCoeff;
    float bodyAttackSamples;
    float bodyHoldSamples;
    float bodyReleaseCoeff;
    float bendReleaseCoeff;
    float bendAmt;           // 0..3 scale (100 → 3.0, so 4× at max)
    float noiseAttackCoeff;
    float noiseAttackSamples;
    float noiseHoldSamples;
    float noiseReleaseCoeff;
    float tilt;              // -1..1
    float tiltCoeff;         // one-pole LP coeff for tilt EQ

    SnarrAlg(SnarrAlg_DTC* d) : dtc(d),
        freqHz(200.0f),
        bodyAttackCoeff(0.2f), bodyAttackSamples(240.0f), bodyHoldSamples(480.0f),
        bodyReleaseCoeff(0.9998f), bendReleaseCoeff(0.9994f), bendAmt(1.5f),
        noiseAttackCoeff(1.0f),  noiseAttackSamples(48.0f),  noiseHoldSamples(240.0f),
        noiseReleaseCoeff(0.9998f), tilt(0.0f), tiltCoeff(0.065f) {}
};

// ---------------------------------------------------------------------------
// Coefficient helpers
// ---------------------------------------------------------------------------

// One-pole attack coefficient — logarithmic shape (fast start, slow approach)
// Reaches ~1 time-constant (63%) in ms milliseconds
static inline float msAttackCoeff(float ms, float sr) {
    const float samps = ms * 0.001f * sr;
    return (samps > 1.0f) ? (1.0f - expf(-1.0f / samps)) : 1.0f;
}

// Exponential decay coefficient — reaches -60 dB in ms milliseconds
static inline float msDecayCoeff(float ms, float sr) {
    const float samps = ms * 0.001f * sr;
    return (samps > 1.0f) ? expf(-6.908f / samps) : 0.0f;
}

// ---------------------------------------------------------------------------
// calculateRequirements
// ---------------------------------------------------------------------------
void calculateRequirements(_NT_algorithmRequirements& req, const int32_t*) {
    req.numParameters = kNumParams;
    req.sram = sizeof(SnarrAlg);
    req.dtc  = sizeof(SnarrAlg_DTC);
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
    SnarrAlg_DTC* d = (SnarrAlg_DTC*)ptrs.dtc;
    d->sinePhase    = 0.0f;
    d->bodyEnv      = 0.0f;
    d->bodyState    = kEnvIdle;
    d->bodyCounter  = 0.0f;
    d->bendEnv      = 0.0f;
    d->noiseEnv     = 0.0f;
    d->noiseState   = kEnvIdle;
    d->noiseCounter = 0.0f;
    d->noiseLP      = 0.0f;
    d->noiseSeed    = 0xDEADBEEFu;
    d->prevTrig     = 0.0f;

    SnarrAlg* a = new (ptrs.sram) SnarrAlg(d);
    a->parameters     = parameters;
    a->parameterPages = &paramPages;

    // Initialise all cached values from parameter defaults
    const float sr = NT_globals.sampleRate;
    a->freqHz             = (float)a->v[kParamFreq];
    a->bodyAttackSamples  = a->v[kParamBAttack] * 0.001f * sr;
    a->bodyAttackCoeff    = msAttackCoeff(a->v[kParamBAttack], sr);
    a->bodyHoldSamples    = a->v[kParamBHold]   * 0.001f * sr;
    a->bodyReleaseCoeff   = msDecayCoeff(a->v[kParamBRel],     sr);
    a->bendReleaseCoeff   = msDecayCoeff(a->v[kParamBendDecay],sr);
    a->bendAmt            = a->v[kParamBendAmt] * 0.12f;
    a->noiseAttackSamples = a->v[kParamNAttack] * 0.001f * sr;
    a->noiseAttackCoeff   = msAttackCoeff(a->v[kParamNAttack], sr);
    a->noiseHoldSamples   = a->v[kParamNHold]   * 0.001f * sr;
    a->noiseReleaseCoeff  = msDecayCoeff(a->v[kParamNRel],     sr);
    a->tilt               = a->v[kParamTilt] / 50.0f;
    // Tilt EQ: 1-pole LP centred at ~500 Hz
    a->tiltCoeff          = 1.0f - expf(-2.0f * kSnarrPi * 500.0f / sr);

    return a;
}

// ---------------------------------------------------------------------------
// parameterChanged
// ---------------------------------------------------------------------------
void parameterChanged(_NT_algorithm* self, int p) {
    SnarrAlg* a = (SnarrAlg*)self;
    const float sr = NT_globals.sampleRate;
    switch (p) {
    case kParamFreq:
        a->freqHz = (float)a->v[kParamFreq];
        break;
    case kParamBAttack:
        a->bodyAttackSamples = a->v[kParamBAttack] * 0.001f * sr;
        a->bodyAttackCoeff   = msAttackCoeff(a->v[kParamBAttack], sr);
        break;
    case kParamBHold:
        a->bodyHoldSamples = a->v[kParamBHold] * 0.001f * sr;
        break;
    case kParamBRel:
        a->bodyReleaseCoeff = msDecayCoeff(a->v[kParamBRel], sr);
        break;
    case kParamBendDecay:
        a->bendReleaseCoeff = msDecayCoeff(a->v[kParamBendDecay], sr);
        break;
    case kParamBendAmt:
        a->bendAmt = a->v[kParamBendAmt] * 0.12f;
        break;
    case kParamNAttack:
        a->noiseAttackSamples = a->v[kParamNAttack] * 0.001f * sr;
        a->noiseAttackCoeff   = msAttackCoeff(a->v[kParamNAttack], sr);
        break;
    case kParamNHold:
        a->noiseHoldSamples = a->v[kParamNHold] * 0.001f * sr;
        break;
    case kParamNRel:
        a->noiseReleaseCoeff = msDecayCoeff(a->v[kParamNRel], sr);
        break;
    case kParamTilt:
        a->tilt = a->v[kParamTilt] / 50.0f;
        break;
    }
}

// ---------------------------------------------------------------------------
// step
// ---------------------------------------------------------------------------
void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    SnarrAlg*     a = (SnarrAlg*)self;
    SnarrAlg_DTC* d = a->dtc;
    const int n  = numFramesBy4 * 4;
    const float sr = NT_globals.sampleRate;

    float* out = busFrames + (a->v[kParamOutput] - 1) * n;

    const int    trigBus = a->v[kParamTrigIn];
    const float* trig    = (trigBus > 0) ? busFrames + (trigBus - 1) * n : nullptr;

    // Pull DTC into locals for the inner loop
    float    sinePhase    = d->sinePhase;
    float    bodyEnv      = d->bodyEnv;
    int      bodyState    = d->bodyState;
    float    bodyCounter  = d->bodyCounter;
    float    bendEnv      = d->bendEnv;
    float    noiseEnv     = d->noiseEnv;
    int      noiseState   = d->noiseState;
    float    noiseCounter = d->noiseCounter;
    float    noiseLP      = d->noiseLP;
    uint32_t noiseSeed    = d->noiseSeed;
    float    prevTrig     = d->prevTrig;

    // Cache algo params to avoid dereferencing per sample
    const float freqDt         = a->freqHz / sr;      // base dt per sample
    const float bodyAttCoeff   = a->bodyAttackCoeff;
    const float bodyAttSamps   = a->bodyAttackSamples;
    const float bodyHoldSamps  = a->bodyHoldSamples;
    const float bodyRelCoeff   = a->bodyReleaseCoeff;
    const float bendRelCoeff   = a->bendReleaseCoeff;
    const float bendAmt        = a->bendAmt;
    const float noiseAttCoeff  = a->noiseAttackCoeff;
    const float noiseAttSamps  = a->noiseAttackSamples;
    const float noiseHoldSamps = a->noiseHoldSamples;
    const float noiseRelCoeff  = a->noiseReleaseCoeff;
    const float tilt           = a->tilt;
    const float tiltCoeff      = a->tiltCoeff;

    for (int i = 0; i < n; ++i) {

        // ---- Trigger detection (rising edge at 0.5 V threshold) ----
        if (trig) {
            const float t = trig[i];
            if (prevTrig < 0.5f && t >= 0.5f) {
                // Reset sine phase
                sinePhase    = 0.0f;
                // Restart body amp envelope from zero
                bodyEnv      = 0.0f;
                bodyState    = kEnvAttack;
                bodyCounter  = 0.0f;
                // Bend envelope: instant-on
                bendEnv      = 1.0f;
                // Restart noise amp envelope from zero
                noiseEnv     = 0.0f;
                noiseState   = kEnvAttack;
                noiseCounter = 0.0f;
            }
            prevTrig = t;
        }

        // ---- Body amplitude envelope (AHD: logarithmic A, flat H, exponential R) ----
        switch (bodyState) {
        case kEnvAttack:
            // Logarithmic approach: one-pole filter toward 1.0
            bodyEnv += (1.0f - bodyEnv) * bodyAttCoeff;
            bodyCounter += 1.0f;
            if (bodyCounter >= bodyAttSamps) {
                bodyEnv     = 1.0f;   // clamp to maximum at end of attack
                bodyState   = kEnvHold;
                bodyCounter = 0.0f;
            }
            break;
        case kEnvHold:
            bodyEnv = 1.0f;
            bodyCounter += 1.0f;
            if (bodyCounter >= bodyHoldSamps) {
                bodyState   = kEnvRelease;
                bodyCounter = 0.0f;
            }
            break;
        case kEnvRelease:
            // Exponential decay
            bodyEnv *= bodyRelCoeff;
            if (bodyEnv < 0.00001f) { bodyEnv = 0.0f; bodyState = kEnvIdle; }
            break;
        default:
            break;
        }

        // ---- Bend envelope (instant onset, single exponential decay) ----
        bendEnv *= bendRelCoeff;
        if (bendEnv < 0.00001f) bendEnv = 0.0f;

        // ---- Sine oscillator with pitch bend ----
        // bendAmt = 3 at max → 1 + 3*1 = 4× frequency
        const float dt         = freqDt * (1.0f + bendAmt * bendEnv);
        const float sineSample = sinf(2.0f * kSnarrPi * sinePhase) * 10.0f * bodyEnv;
        sinePhase += dt;
        if (sinePhase >= 1.0f) sinePhase -= 1.0f;

        // ---- Noise amplitude envelope (AHD: same shape as body) ----
        switch (noiseState) {
        case kEnvAttack:
            noiseEnv += (1.0f - noiseEnv) * noiseAttCoeff;
            noiseCounter += 1.0f;
            if (noiseCounter >= noiseAttSamps) {
                noiseEnv     = 1.0f;
                noiseState   = kEnvHold;
                noiseCounter = 0.0f;
            }
            break;
        case kEnvHold:
            noiseEnv = 1.0f;
            noiseCounter += 1.0f;
            if (noiseCounter >= noiseHoldSamps) {
                noiseState   = kEnvRelease;
                noiseCounter = 0.0f;
            }
            break;
        case kEnvRelease:
            noiseEnv *= noiseRelCoeff;
            if (noiseEnv < 0.00001f) { noiseEnv = 0.0f; noiseState = kEnvIdle; }
            break;
        default:
            break;
        }

        // ---- White noise + tilt EQ ----
        // LCG random, mapped to -1..+1
        noiseSeed = noiseSeed * 1664525u + 1013904223u;
        const float rawNoise  = (float)(int32_t)noiseSeed * (1.0f / 2147483648.0f);
        // One-pole LP at ~500 Hz
        noiseLP += tiltCoeff * (rawNoise - noiseLP);
        // Tilt blend: tilt=0 → white, tilt>0 → HP emphasis, tilt<0 → LP emphasis
        // At tilt=-1: output = LP.  At tilt=0: output = white.  At tilt=+1: output = 2×white - LP
        const float tiltedNoise = rawNoise + tilt * (rawNoise - noiseLP);

        const float noiseSample = tiltedNoise * 10.0f * noiseEnv;

        // ---- Mix and add to output bus ----
        out[i] += sineSample + noiseSample;
    }

    // Write locals back to DTC
    d->sinePhase    = sinePhase;
    d->bodyEnv      = bodyEnv;
    d->bodyState    = bodyState;
    d->bodyCounter  = bodyCounter;
    d->bendEnv      = bendEnv;
    d->noiseEnv     = noiseEnv;
    d->noiseState   = noiseState;
    d->noiseCounter = noiseCounter;
    d->noiseLP      = noiseLP;
    d->noiseSeed    = noiseSeed;
    d->prevTrig     = prevTrig;
}

// ---------------------------------------------------------------------------
// Factory and entry point
// ---------------------------------------------------------------------------
static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('S','N','A','R'),
    .name                        = "SNARR",
    .description                 = "Snare drum synthesiser",
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

extern "C" uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
    switch (selector) {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return (uintptr_t)(data == 0 ? &factory : NULL);
    }
    return 0;
}
