#include "distingnt/api.h"
#include <cmath>
#include <cstdint>
#include <new>

static const float kSnarrPi    = 3.14159265358979f;
static const float kSnarrSat   = 50.0f;   // max tanh drive
static const float kSnarrScale = 10.0f;   // saturation voltage scale

// ---------------------------------------------------------------------------
// Envelope state constants
// ---------------------------------------------------------------------------
enum { kEnvIdle = 0, kEnvAttack = 1, kEnvHold = 2, kEnvRelease = 3 };

// ---------------------------------------------------------------------------
// Bandpass biquad coefficient computation
// Direct Form II Transposed — b1=0, b2=-b0 (implicit), only b0/a1/a2 stored
// ---------------------------------------------------------------------------
static void computeBP(float& b0, float& a1, float& a2, float fc, float bw, float sr) {
    if (fc < 30.0f)    fc = 30.0f;
    if (fc > 15000.0f) fc = 15000.0f;
    if (bw < 1.0f)     bw = 1.0f;
    float Q = fc / bw;
    if (Q < 0.1f) Q = 0.1f;
    const float w0    = 2.0f * kSnarrPi * fc / sr;
    const float alpha = sinf(w0) / (2.0f * Q);
    const float cosw0 = cosf(w0);
    const float a0inv = 1.0f / (1.0f + alpha);
    b0 =  alpha         * a0inv;
    a1 = -2.0f * cosw0  * a0inv;
    a2 = (1.0f - alpha) * a0inv;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
enum {
    // Body page
    kParamFreq,        //  0
    kParamBAttack,     //  1
    kParamBHold,       //  2
    kParamBRel,        //  3
    kParamBendDecay,   //  4
    kParamBendAmt,     //  5
    // Noise page
    kParamNAttack,     //  6
    kParamNHold,       //  7
    kParamNRel,        //  8
    kParamTilt,        //  9
    kParamNSat,        // 10
    kParamNBias,       // 11
    kParamNDryWet,     // 12
    kParamNBaseFreq,   // 13
    kParamNSpacing,    // 14
    kParamNBandwidth,  // 15
    kParamNFeedback,   // 16
    // Routing page
    kParamTrigIn,      // 17
    kParamOutput,      // 18
    kNumParams         // 19
};

static const _NT_parameter parameters[kNumParams] = {
    // Body page (0-5)
    { .name="Freq",      .min=50,  .max=2000,  .def=200,
      .unit=kNT_unitHz,      .scaling=0,              .enumStrings=NULL },
    { .name="BAttack",   .min=1,   .max=500,   .def=5,
      .unit=kNT_unitMs,      .scaling=0,              .enumStrings=NULL },
    { .name="BHold",     .min=1,   .max=500,   .def=10,
      .unit=kNT_unitMs,      .scaling=0,              .enumStrings=NULL },
    { .name="BRel",      .min=1,   .max=500,   .def=150,
      .unit=kNT_unitMs,      .scaling=0,              .enumStrings=NULL },
    { .name="BendDecay", .min=1,   .max=500,   .def=80,
      .unit=kNT_unitMs,      .scaling=0,              .enumStrings=NULL },
    { .name="BendAmt",   .min=0,   .max=100,   .def=50,
      .unit=kNT_unitPercent, .scaling=0,              .enumStrings=NULL },
    // Noise page (6-16)
    { .name="NAttack",   .min=1,   .max=500,   .def=1,
      .unit=kNT_unitMs,      .scaling=0,              .enumStrings=NULL },
    { .name="NHold",     .min=1,   .max=500,   .def=5,
      .unit=kNT_unitMs,      .scaling=0,              .enumStrings=NULL },
    { .name="NRel",      .min=1,   .max=500,   .def=200,
      .unit=kNT_unitMs,      .scaling=0,              .enumStrings=NULL },
    { .name="Tilt",      .min=-50, .max=50,    .def=0,
      .unit=kNT_unitNone,    .scaling=0,              .enumStrings=NULL },
    { .name="Sat",       .min=0,   .max=100,   .def=0,
      .unit=kNT_unitPercent, .scaling=0,              .enumStrings=NULL },
    { .name="Bias",      .min=-50, .max=50,    .def=0,
      .unit=kNT_unitVolts,   .scaling=kNT_scaling10,  .enumStrings=NULL },
    { .name="Dry/Wet",   .min=0,   .max=100,   .def=0,
      .unit=kNT_unitPercent, .scaling=0,              .enumStrings=NULL },
    { .name="BaseFreq",  .min=30,  .max=15000, .def=1000,
      .unit=kNT_unitHz,      .scaling=0,              .enumStrings=NULL },
    { .name="Spacing",   .min=0,   .max=15000, .def=0,
      .unit=kNT_unitHz,      .scaling=0,              .enumStrings=NULL },
    { .name="Bandwidth", .min=10,  .max=1000,  .def=200,
      .unit=kNT_unitHz,      .scaling=0,              .enumStrings=NULL },
    { .name="Feedback",  .min=0,   .max=100,   .def=0,
      .unit=kNT_unitPercent, .scaling=0,              .enumStrings=NULL },
    // Routing page (17-18)
    NT_PARAMETER_CV_INPUT(  "TrigIn", 1, 1 )
    NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )
};

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
static const uint8_t bodyParams[]    = { kParamFreq, kParamBAttack, kParamBHold,
                                          kParamBRel, kParamBendDecay, kParamBendAmt };
static const uint8_t noiseParams[]   = { kParamNAttack, kParamNHold, kParamNRel, kParamTilt,
                                          kParamNSat, kParamNBias, kParamNDryWet,
                                          kParamNBaseFreq, kParamNSpacing, kParamNBandwidth,
                                          kParamNFeedback };
static const uint8_t routingParams[] = { kParamTrigIn, kParamOutput };

static const _NT_parameterPage pages[] = {
    { .name="Body",    .numParams=6,  .group=0, .params=bodyParams    },
    { .name="Noise",   .numParams=11, .group=0, .params=noiseParams   },
    { .name="Routing", .numParams=2,  .group=0, .params=routingParams },
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
    // Noise bandpass state: 3 parallel biquads
    float    bpX1[3], bpX2[3];
    float    bpY1[3], bpY2[3];
    float    prevBpOut;
    // Noise LCG seed
    uint32_t noiseSeed;
    // Trigger edge detection
    float    prevTrig;
};

struct SnarrAlg : public _NT_algorithm {
    SnarrAlg_DTC* dtc;
    // Body cached values
    float freqHz;
    float bodyAttackCoeff;
    float bodyAttackSamples;
    float bodyHoldSamples;
    float bodyReleaseCoeff;
    float bendReleaseCoeff;
    float bendAmt;
    // Noise envelope cached values
    float noiseAttackCoeff;
    float noiseAttackSamples;
    float noiseHoldSamples;
    float noiseReleaseCoeff;
    float tilt;
    float tiltCoeff;
    // Noise bandpass coefficients (3 parallel biquads)
    float bpB0[3], bpA1[3], bpA2[3];
    // Noise saturator cached values
    float satDrive;
    float satBias;
    float satDryWet;
    float satFeedback;

    SnarrAlg(SnarrAlg_DTC* d) : dtc(d),
        freqHz(200.0f),
        bodyAttackCoeff(0.2f), bodyAttackSamples(240.0f), bodyHoldSamples(480.0f),
        bodyReleaseCoeff(0.9998f), bendReleaseCoeff(0.9994f), bendAmt(6.0f),
        noiseAttackCoeff(1.0f), noiseAttackSamples(48.0f), noiseHoldSamples(240.0f),
        noiseReleaseCoeff(0.9998f), tilt(0.0f), tiltCoeff(0.065f),
        satDrive(0.0f), satBias(0.0f), satDryWet(0.0f), satFeedback(0.0f)
    {
        for (int i = 0; i < 3; ++i) {
            bpB0[i] = 0.0f; bpA1[i] = 0.0f; bpA2[i] = 0.0f;
        }
    }
};

// ---------------------------------------------------------------------------
// Coefficient helpers
// ---------------------------------------------------------------------------
static inline float msAttackCoeff(float ms, float sr) {
    const float samps = ms * 0.001f * sr;
    return (samps > 1.0f) ? (1.0f - expf(-1.0f / samps)) : 1.0f;
}

static inline float msDecayCoeff(float ms, float sr) {
    const float samps = ms * 0.001f * sr;
    return (samps > 1.0f) ? expf(-6.908f / samps) : 0.0f;
}

static void updateNoiseBP(SnarrAlg* a) {
    const float sr      = NT_globals.sampleRate;
    const float base    = (float)a->v[kParamNBaseFreq];
    const float spacing = (float)a->v[kParamNSpacing];
    const float bw      = (float)a->v[kParamNBandwidth];
    float fc0 = base;
    float fc1 = base + spacing;
    float fc2 = base + 2.0f * spacing;
    const float maxFreq = 15000.0f;
    if (fc0 > maxFreq) fc0 = maxFreq;
    if (fc1 > maxFreq) fc1 = maxFreq;
    if (fc2 > maxFreq) fc2 = maxFreq;
    computeBP(a->bpB0[0], a->bpA1[0], a->bpA2[0], fc0, bw, sr);
    computeBP(a->bpB0[1], a->bpA1[1], a->bpA2[1], fc1, bw, sr);
    computeBP(a->bpB0[2], a->bpA1[2], a->bpA2[2], fc2, bw, sr);
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
    for (int i = 0; i < 3; ++i) {
        d->bpX1[i] = 0.0f; d->bpX2[i] = 0.0f;
        d->bpY1[i] = 0.0f; d->bpY2[i] = 0.0f;
    }
    d->prevBpOut    = 0.0f;
    d->noiseSeed    = 0xDEADBEEFu;
    d->prevTrig     = 0.0f;

    SnarrAlg* a = new (ptrs.sram) SnarrAlg(d);
    a->parameters     = parameters;
    a->parameterPages = &paramPages;

    const float sr = NT_globals.sampleRate;
    a->freqHz             = (float)a->v[kParamFreq];
    a->bodyAttackSamples  = a->v[kParamBAttack]  * 0.001f * sr;
    a->bodyAttackCoeff    = msAttackCoeff(a->v[kParamBAttack],  sr);
    a->bodyHoldSamples    = a->v[kParamBHold]    * 0.001f * sr;
    a->bodyReleaseCoeff   = msDecayCoeff(a->v[kParamBRel],      sr);
    a->bendReleaseCoeff   = msDecayCoeff(a->v[kParamBendDecay], sr);
    a->bendAmt            = a->v[kParamBendAmt]  * 0.12f;
    a->noiseAttackSamples = a->v[kParamNAttack]  * 0.001f * sr;
    a->noiseAttackCoeff   = msAttackCoeff(a->v[kParamNAttack],  sr);
    a->noiseHoldSamples   = a->v[kParamNHold]    * 0.001f * sr;
    a->noiseReleaseCoeff  = msDecayCoeff(a->v[kParamNRel],      sr);
    a->tilt               = a->v[kParamTilt]     / 50.0f;
    a->tiltCoeff          = 1.0f - expf(-2.0f * kSnarrPi * 500.0f / sr);
    a->satDrive           = a->v[kParamNSat]     / 100.0f * kSnarrSat;
    a->satBias            = a->v[kParamNBias]    / 100.0f;
    a->satDryWet          = a->v[kParamNDryWet]  / 100.0f;
    a->satFeedback        = a->v[kParamNFeedback]/ 100.0f;
    updateNoiseBP(a);

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
    case kParamNSat:
        a->satDrive = a->v[kParamNSat] / 100.0f * kSnarrSat;
        break;
    case kParamNBias:
        a->satBias = a->v[kParamNBias] / 100.0f;
        break;
    case kParamNDryWet:
        a->satDryWet = a->v[kParamNDryWet] / 100.0f;
        break;
    case kParamNBaseFreq:
    case kParamNSpacing:
    case kParamNBandwidth:
        updateNoiseBP(a);
        break;
    case kParamNFeedback:
        a->satFeedback = a->v[kParamNFeedback] / 100.0f;
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

    // Pull DTC into locals
    float    sinePhase    = d->sinePhase;
    float    bodyEnv      = d->bodyEnv;
    int      bodyState    = d->bodyState;
    float    bodyCounter  = d->bodyCounter;
    float    bendEnv      = d->bendEnv;
    float    noiseEnv     = d->noiseEnv;
    int      noiseState   = d->noiseState;
    float    noiseCounter = d->noiseCounter;
    float    noiseLP      = d->noiseLP;
    float    x1_0=d->bpX1[0], x2_0=d->bpX2[0], y1_0=d->bpY1[0], y2_0=d->bpY2[0];
    float    x1_1=d->bpX1[1], x2_1=d->bpX2[1], y1_1=d->bpY1[1], y2_1=d->bpY2[1];
    float    x1_2=d->bpX1[2], x2_2=d->bpX2[2], y1_2=d->bpY1[2], y2_2=d->bpY2[2];
    float    prevBpOut    = d->prevBpOut;
    uint32_t noiseSeed    = d->noiseSeed;
    float    prevTrig     = d->prevTrig;

    // Cache algo params
    const float freqDt         = a->freqHz / sr;
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

    // Bandpass coefficients (block-constant)
    const float b0_0=a->bpB0[0], a1_0=a->bpA1[0], a2_0=a->bpA2[0];
    const float b0_1=a->bpB0[1], a1_1=a->bpA1[1], a2_1=a->bpA2[1];
    const float b0_2=a->bpB0[2], a1_2=a->bpA1[2], a2_2=a->bpA2[2];

    // Saturator block-constant values
    const float satDrive    = a->satDrive;
    const float satBias     = a->satBias;
    const float satFeedback = a->satFeedback;
    const float satDryWet   = a->satDryWet;
    const bool  doSat       = (satDrive >= 0.001f);
    const float tanhDrive   = doSat ? tanhf(satDrive) : 1.0f;
    const float biasComp    = doSat ? tanhf(satDrive * satBias) / tanhDrive : 0.0f;
    // Equal-power dry/wet
    const float wetCoeff    = sinf(satDryWet * kSnarrPi * 0.5f);
    const float dryCoeff    = cosf(satDryWet * kSnarrPi * 0.5f);

    for (int i = 0; i < n; ++i) {

        // ---- Trigger detection (rising edge at 0.5 V threshold) ----
        if (trig) {
            const float t = trig[i];
            if (prevTrig < 0.5f && t >= 0.5f) {
                sinePhase    = 0.0f;
                bodyEnv      = 0.0f;
                bodyState    = kEnvAttack;
                bodyCounter  = 0.0f;
                bendEnv      = 1.0f;
                noiseEnv     = 0.0f;
                noiseState   = kEnvAttack;
                noiseCounter = 0.0f;
            }
            prevTrig = t;
        }

        // ---- Body amplitude envelope (logarithmic attack, exp release) ----
        switch (bodyState) {
        case kEnvAttack:
            bodyEnv += (1.0f - bodyEnv) * bodyAttCoeff;
            bodyCounter += 1.0f;
            if (bodyCounter >= bodyAttSamps) {
                bodyEnv = 1.0f; bodyState = kEnvHold; bodyCounter = 0.0f;
            }
            break;
        case kEnvHold:
            bodyEnv = 1.0f;
            bodyCounter += 1.0f;
            if (bodyCounter >= bodyHoldSamps) { bodyState = kEnvRelease; bodyCounter = 0.0f; }
            break;
        case kEnvRelease:
            bodyEnv *= bodyRelCoeff;
            if (bodyEnv < 0.00001f) { bodyEnv = 0.0f; bodyState = kEnvIdle; }
            break;
        default: break;
        }

        // ---- Bend envelope (instant onset, exp decay) ----
        bendEnv *= bendRelCoeff;
        if (bendEnv < 0.00001f) bendEnv = 0.0f;

        // ---- Sine oscillator with pitch bend ----
        const float dt         = freqDt * (1.0f + bendAmt * bendEnv);
        const float sineSample = sinf(2.0f * kSnarrPi * sinePhase) * 10.0f * bodyEnv;
        sinePhase += dt;
        if (sinePhase >= 1.0f) sinePhase -= 1.0f;

        // ---- Noise amplitude envelope ----
        switch (noiseState) {
        case kEnvAttack:
            noiseEnv += (1.0f - noiseEnv) * noiseAttCoeff;
            noiseCounter += 1.0f;
            if (noiseCounter >= noiseAttSamps) {
                noiseEnv = 1.0f; noiseState = kEnvHold; noiseCounter = 0.0f;
            }
            break;
        case kEnvHold:
            noiseEnv = 1.0f;
            noiseCounter += 1.0f;
            if (noiseCounter >= noiseHoldSamps) { noiseState = kEnvRelease; noiseCounter = 0.0f; }
            break;
        case kEnvRelease:
            noiseEnv *= noiseRelCoeff;
            if (noiseEnv < 0.00001f) { noiseEnv = 0.0f; noiseState = kEnvIdle; }
            break;
        default: break;
        }

        // ---- White noise + tilt EQ + amplitude envelope ----
        noiseSeed = noiseSeed * 1664525u + 1013904223u;
        const float rawNoise  = (float)(int32_t)noiseSeed * (1.0f / 2147483648.0f);
        noiseLP += tiltCoeff * (rawNoise - noiseLP);
        const float tiltedNoise = rawNoise + tilt * (rawNoise - noiseLP);
        const float noiseDry    = tiltedNoise * 10.0f * noiseEnv;  // pre-saturator signal

        // ---- Noise saturator ----
        // Feedback from previous bandpass output feeds into sat input
        const float satInput = noiseDry + satFeedback * prevBpOut;
        float satOut;
        if (doSat) {
            const float norm   = satInput / kSnarrScale;
            const float biased = norm + satBias;
            satOut = (tanhf(satDrive * biased) / tanhDrive - biasComp) * kSnarrScale;
        } else {
            satOut = satInput;
        }

        // ---- Triple parallel bandpass (Direct Form II Transposed) ----
        // b2 = -b0 is implicit; b1 = 0
        float bp0 = b0_0 * satOut - b0_0 * x2_0 - a1_0 * y1_0 - a2_0 * y2_0;
        x2_0=x1_0; x1_0=satOut; y2_0=y1_0; y1_0=bp0;

        float bp1 = b0_1 * satOut - b0_1 * x2_1 - a1_1 * y1_1 - a2_1 * y2_1;
        x2_1=x1_1; x1_1=satOut; y2_1=y1_1; y1_1=bp1;

        float bp2 = b0_2 * satOut - b0_2 * x2_2 - a1_2 * y1_2 - a2_2 * y2_2;
        x2_2=x1_2; x1_2=satOut; y2_2=y1_2; y1_2=bp2;

        prevBpOut = (bp0 + bp1 + bp2) / 3.0f;

        // ---- Equal-power dry/wet blend ----
        // Dry = post-envelope noise before saturation; Wet = bandpass output
        const float noiseSample = dryCoeff * noiseDry + wetCoeff * prevBpOut;

        // ---- Sum to output bus ----
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
    d->bpX1[0]=x1_0; d->bpX2[0]=x2_0; d->bpY1[0]=y1_0; d->bpY2[0]=y2_0;
    d->bpX1[1]=x1_1; d->bpX2[1]=x2_1; d->bpY1[1]=y1_1; d->bpY2[1]=y2_1;
    d->bpX1[2]=x1_2; d->bpX2[2]=x2_2; d->bpY1[2]=y1_2; d->bpY2[2]=y2_2;
    d->prevBpOut    = prevBpOut;
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
