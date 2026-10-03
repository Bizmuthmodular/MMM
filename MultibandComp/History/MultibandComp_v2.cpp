#include "distingnt/api.h"
#include <cmath>
#include <cstring>
#include <new>

// ─── Constants ───────────────────────────────────────────────────────────────
static const float kPi = 3.14159265358979f;

enum { kBandLow, kBandMid, kBandHigh, kNumBands };

static const char* const kOnOffNames[] = { "OFF", "ON", NULL };

// ─── DTC ─────────────────────────────────────────────────────────────────────
// Per-sample state: two cascaded crossover biquad stages (Mid-High, then
// Low-Mid applied to the Mid-High stage's lowpass output), each with
// independent L/R filter state, plus per-band compressor gain-reduction
// state (one pair per band, persists between samples).
struct MultibandCompAlg_DTC {
    float xoHiSL1, xoHiSL2, xoHiSR1, xoHiSR2;   // Mid-High crossover stage
    float xoLoSL1, xoLoSL2, xoLoSR1, xoLoSR2;   // Low-Mid crossover stage
    float compL[kNumBands];
    float compR[kNumBands];
};

// ─── Algorithm struct ────────────────────────────────────────────────────────
struct MultibandCompAlg : public _NT_algorithm {
    MultibandCompAlg_DTC* dtc;

    // Crossover biquad coefficients (Q=1 Butterworth lowpass, bilinear K-method)
    float xoHiA0, xoHiA1, xoHiA2, xoHiB1, xoHiB2;
    float xoLoA0, xoLoA1, xoLoA2, xoLoB1, xoLoB2;

    // Cached per-band dynamics coefficients
    float thresh[kNumBands];
    float ratio[kNumBands];
    float attack[kNumBands];
    float release[kNumBands];
    float gainLin[kNumBands];

    float preGain;       // linear, from "Pre/post gain" dB parameter
    float overallscale;  // sampleRate / 44100, fixed for the algorithm's lifetime

    // Metering: ~1s exponential moving average of each band's linear gain
    // factor (1.0 = no reduction), read by draw() and converted to dB there.
    float grAvgLin[kNumBands];
    float meterCoeff;

    explicit MultibandCompAlg(MultibandCompAlg_DTC* d)
        : dtc(d),
          xoHiA0(0.0f), xoHiA1(0.0f), xoHiA2(0.0f), xoHiB1(0.0f), xoHiB2(0.0f),
          xoLoA0(0.0f), xoLoA1(0.0f), xoLoA2(0.0f), xoLoB1(0.0f), xoLoB2(0.0f),
          preGain(1.0f), overallscale(1.0f), meterCoeff(1.0f)
    {
        for (int b = 0; b < kNumBands; ++b) {
            thresh[b]   = 1.0f;
            ratio[b]    = 0.0f;
            attack[b]   = 0.001f;
            release[b]  = 0.001f;
            gainLin[b]  = 1.0f;
            grAvgLin[b] = 1.0f;
        }
    }
};

// ─── Parameters ──────────────────────────────────────────────────────────────
enum {
    kParamInputL, kParamInputR,
    kParamOutputL, kParamOutputLMode, kParamOutputR, kParamOutputRMode,
    kParamLowOutL, kParamLowOutLMode, kParamLowOutR, kParamLowOutRMode,
    kParamMidOutL, kParamMidOutLMode, kParamMidOutR, kParamMidOutRMode,
    kParamHighOutL, kParamHighOutLMode, kParamHighOutR, kParamHighOutRMode,
    kParamLowMidFreq, kParamMidHighFreq,
    kParamLowThresh, kParamLowRatio, kParamLowAttack, kParamLowRelease, kParamLowGain, kParamLowSolo,
    kParamMidThresh, kParamMidRatio, kParamMidAttack, kParamMidRelease, kParamMidGain, kParamMidSolo,
    kParamHighThresh, kParamHighRatio, kParamHighAttack, kParamHighRelease, kParamHighGain, kParamHighSolo,
    kParamPrePostGain, kParamBypass,
    kNumParams
};

static const _NT_parameter parameters[] = {
    NT_PARAMETER_AUDIO_INPUT("Input L", 1, 1)
    NT_PARAMETER_AUDIO_INPUT("Input R", 1, 2)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Output L", 1, 13)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Output R", 1, 14)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Low Out L", 0, 0)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Low Out R", 0, 0)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Mid Out L", 0, 0)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Mid Out R", 0, 0)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("High Out L", 0, 0)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("High Out R", 0, 0)

    { .name = "Low-Mid Freq",  .min = 0, .max = 1000, .def = 341,
      .unit = kNT_unitNone, .scaling = kNT_scalingNone, .enumStrings = NULL },
    { .name = "Mid-High Freq", .min = 0, .max = 1000, .def = 786,
      .unit = kNT_unitNone, .scaling = kNT_scalingNone, .enumStrings = NULL },

    { .name = "Low Thresh",  .min = -40, .max = 0, .def = 0,
      .unit = kNT_unitDb, .scaling = kNT_scalingNone, .enumStrings = NULL },
    { .name = "Low Ratio",   .min = 0, .max = 1000, .def = 500,
      .unit = kNT_unitNone, .scaling = kNT_scaling1000, .enumStrings = NULL },
    { .name = "Low Attack",  .min = 0, .max = 1000, .def = 500,
      .unit = kNT_unitNone, .scaling = kNT_scaling1000, .enumStrings = NULL },
    { .name = "Low Release", .min = 0, .max = 1000, .def = 500,
      .unit = kNT_unitNone, .scaling = kNT_scaling1000, .enumStrings = NULL },
    { .name = "Low Gain",    .min = -24, .max = 24, .def = 0,
      .unit = kNT_unitDb, .scaling = kNT_scalingNone, .enumStrings = NULL },
    { .name = "Low Solo",    .min = 0, .max = 1, .def = 0,
      .unit = kNT_unitEnum, .scaling = kNT_scalingNone, .enumStrings = kOnOffNames },

    { .name = "Mid Thresh",  .min = -40, .max = 0, .def = 0,
      .unit = kNT_unitDb, .scaling = kNT_scalingNone, .enumStrings = NULL },
    { .name = "Mid Ratio",   .min = 0, .max = 1000, .def = 500,
      .unit = kNT_unitNone, .scaling = kNT_scaling1000, .enumStrings = NULL },
    { .name = "Mid Attack",  .min = 0, .max = 1000, .def = 500,
      .unit = kNT_unitNone, .scaling = kNT_scaling1000, .enumStrings = NULL },
    { .name = "Mid Release", .min = 0, .max = 1000, .def = 500,
      .unit = kNT_unitNone, .scaling = kNT_scaling1000, .enumStrings = NULL },
    { .name = "Mid Gain",    .min = -24, .max = 24, .def = 0,
      .unit = kNT_unitDb, .scaling = kNT_scalingNone, .enumStrings = NULL },
    { .name = "Mid Solo",    .min = 0, .max = 1, .def = 0,
      .unit = kNT_unitEnum, .scaling = kNT_scalingNone, .enumStrings = kOnOffNames },

    { .name = "High Thresh",  .min = -40, .max = 0, .def = 0,
      .unit = kNT_unitDb, .scaling = kNT_scalingNone, .enumStrings = NULL },
    { .name = "High Ratio",   .min = 0, .max = 1000, .def = 500,
      .unit = kNT_unitNone, .scaling = kNT_scaling1000, .enumStrings = NULL },
    { .name = "High Attack",  .min = 0, .max = 1000, .def = 500,
      .unit = kNT_unitNone, .scaling = kNT_scaling1000, .enumStrings = NULL },
    { .name = "High Release", .min = 0, .max = 1000, .def = 500,
      .unit = kNT_unitNone, .scaling = kNT_scaling1000, .enumStrings = NULL },
    { .name = "High Gain",    .min = -24, .max = 24, .def = 0,
      .unit = kNT_unitDb, .scaling = kNT_scalingNone, .enumStrings = NULL },
    { .name = "High Solo",    .min = 0, .max = 1, .def = 0,
      .unit = kNT_unitEnum, .scaling = kNT_scalingNone, .enumStrings = kOnOffNames },

    { .name = "Pre/post gain", .min = -36, .max = 0, .def = -20,
      .unit = kNT_unitDb, .scaling = kNT_scalingNone, .enumStrings = NULL },
    { .name = "Bypass", .min = 0, .max = 1, .def = 0,
      .unit = kNT_unitEnum, .scaling = kNT_scalingNone, .enumStrings = kOnOffNames },
};

// ─── Pages ───────────────────────────────────────────────────────────────────
static const uint8_t routingParams[] = {
    kParamInputL, kParamInputR, kParamOutputL, kParamOutputLMode, kParamOutputR, kParamOutputRMode
};
static const uint8_t bandOutParams[] = {
    kParamLowOutL, kParamLowOutLMode, kParamLowOutR, kParamLowOutRMode,
    kParamMidOutL, kParamMidOutLMode, kParamMidOutR, kParamMidOutRMode,
    kParamHighOutL, kParamHighOutLMode, kParamHighOutR, kParamHighOutRMode
};
static const uint8_t crossoverParams[] = { kParamLowMidFreq, kParamMidHighFreq };
static const uint8_t lowBandParams[]  = { kParamLowThresh,  kParamLowRatio,  kParamLowAttack,  kParamLowRelease,  kParamLowGain,  kParamLowSolo };
static const uint8_t midBandParams[]  = { kParamMidThresh,  kParamMidRatio,  kParamMidAttack,  kParamMidRelease,  kParamMidGain,  kParamMidSolo };
static const uint8_t highBandParams[] = { kParamHighThresh, kParamHighRatio, kParamHighAttack, kParamHighRelease, kParamHighGain, kParamHighSolo };
static const uint8_t masterParams[]   = { kParamPrePostGain, kParamBypass };

static const _NT_parameterPage pages[] = {
    { .name = "Routing",   .numParams = ARRAY_SIZE(routingParams),   .group = 0, .params = routingParams },
    { .name = "Band Outs", .numParams = ARRAY_SIZE(bandOutParams),   .group = 0, .params = bandOutParams },
    { .name = "Crossover", .numParams = ARRAY_SIZE(crossoverParams), .group = 0, .params = crossoverParams },
    { .name = "Low Band",  .numParams = ARRAY_SIZE(lowBandParams),   .group = 0, .params = lowBandParams },
    { .name = "Mid Band",  .numParams = ARRAY_SIZE(midBandParams),   .group = 0, .params = midBandParams },
    { .name = "High Band", .numParams = ARRAY_SIZE(highBandParams),  .group = 0, .params = highBandParams },
    { .name = "Master",    .numParams = ARRAY_SIZE(masterParams),    .group = 0, .params = masterParams },
};
static const _NT_parameterPages paramPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages    = pages,
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

// 0-1000 raw -> 20 Hz-17 kHz log (same mapping as DualPhaser.cpp's paramToFreq)
static inline float paramToFreq(int v) {
    return 20.0f * powf(850.0f, (float)v / 1000.0f);
}

// Q=1 Butterworth lowpass biquad, bilinear K-method (ported from SmoothEQ3.cpp
// stage 1 - see Disting NT Examples/distingNT_API/airwindows/src/SmoothEQ3.cpp:97-110)
static void recalcCrossoverStage(float freqHz, float sampleRate,
                                  float& a0, float& a1, float& a2, float& b1, float& b2) {
    const float K    = tanf(kPi * freqHz / sampleRate);
    const float norm = 1.0f / (1.0f + K + K * K);
    a0 = K * K * norm;
    a1 = 2.0f * a0;
    a2 = a0;
    b1 = 2.0f * (K * K - 1.0f) * norm;
    b2 = (1.0f - K + K * K) * norm;
}

static void recalcCrossover(MultibandCompAlg* a) {
    const float sr = (float)NT_globals.sampleRate;
    recalcCrossoverStage(paramToFreq(a->v[kParamMidHighFreq]), sr,
                          a->xoHiA0, a->xoHiA1, a->xoHiA2, a->xoHiB1, a->xoHiB2);
    recalcCrossoverStage(paramToFreq(a->v[kParamLowMidFreq]), sr,
                          a->xoLoA0, a->xoLoA1, a->xoLoA2, a->xoLoB1, a->xoLoB2);
}

// Compressor coefficient mapping ported from Pop3.cpp:64-72 (gate section dropped).
// Threshold is now a real dB parameter (-40..0), converted to the linear
// domain the ported math operates in.
static void recalcBand(MultibandCompAlg* a, int band,
                        int pThresh, int pRatio, int pAttack, int pRelease, int pGain) {
    const float threshDb = (float)a->v[pThresh];
    const float r        = (float)a->v[pRatio]   / 1000.0f;
    const float atk      = (float)a->v[pAttack]  / 1000.0f;
    const float rel      = (float)a->v[pRelease] / 1000.0f;
    a->thresh[band]  = powf(10.0f, threshDb / 20.0f);
    a->ratio[band]   = 1.0f - powf(1.0f - r, 2.0f);
    a->attack[band]  = 1.0f / ((powf(atk, 3.0f) * 5000.0f + 500.0f) * a->overallscale);
    a->release[band] = 1.0f / ((powf(rel, 5.0f) * 50000.0f + 500.0f) * a->overallscale);
    a->gainLin[band] = powf(10.0f, (float)a->v[pGain] / 20.0f);
}

static void recalcPreGain(MultibandCompAlg* a) {
    a->preGain = powf(10.0f, (float)a->v[kParamPrePostGain] / 20.0f);
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t*) {
    req.numParameters = ARRAY_SIZE(parameters);
    req.sram = sizeof(MultibandCompAlg);
    req.dram = 0;
    req.dtc  = sizeof(MultibandCompAlg_DTC);
    req.itc  = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs,
                          const _NT_algorithmRequirements&, const int32_t*) {
    MultibandCompAlg_DTC* d = (MultibandCompAlg_DTC*)ptrs.dtc;
    memset(d, 0, sizeof(*d));
    // Compressor gain-reduction state starts at "no reduction" (1.0), matching
    // Pop3.cpp's reset() - starting at 0 would slam the gain on the first samples.
    for (int b = 0; b < kNumBands; ++b) {
        d->compL[b] = 1.0f;
        d->compR[b] = 1.0f;
    }

    MultibandCompAlg* a = new (ptrs.sram) MultibandCompAlg(d);
    a->parameters     = parameters;
    a->parameterPages = &paramPages;

    a->overallscale = (float)NT_globals.sampleRate / 44100.0f;
    a->meterCoeff   = 1.0f / (float)NT_globals.sampleRate;  // ~1s EMA time constant
    recalcCrossover(a);
    recalcBand(a, kBandLow,  kParamLowThresh,  kParamLowRatio,  kParamLowAttack,  kParamLowRelease,  kParamLowGain);
    recalcBand(a, kBandMid,  kParamMidThresh,  kParamMidRatio,  kParamMidAttack,  kParamMidRelease,  kParamMidGain);
    recalcBand(a, kBandHigh, kParamHighThresh, kParamHighRatio, kParamHighAttack, kParamHighRelease, kParamHighGain);
    recalcPreGain(a);

    return a;
}

void parameterChanged(_NT_algorithm* self, int p) {
    MultibandCompAlg* a = (MultibandCompAlg*)self;
    switch (p) {
    case kParamLowMidFreq:
    case kParamMidHighFreq:
        recalcCrossover(a);
        break;
    case kParamLowThresh: case kParamLowRatio: case kParamLowAttack: case kParamLowRelease: case kParamLowGain:
        recalcBand(a, kBandLow, kParamLowThresh, kParamLowRatio, kParamLowAttack, kParamLowRelease, kParamLowGain);
        break;
    case kParamMidThresh: case kParamMidRatio: case kParamMidAttack: case kParamMidRelease: case kParamMidGain:
        recalcBand(a, kBandMid, kParamMidThresh, kParamMidRatio, kParamMidAttack, kParamMidRelease, kParamMidGain);
        break;
    case kParamHighThresh: case kParamHighRatio: case kParamHighAttack: case kParamHighRelease: case kParamHighGain:
        recalcBand(a, kBandHigh, kParamHighThresh, kParamHighRatio, kParamHighAttack, kParamHighRelease, kParamHighGain);
        break;
    case kParamPrePostGain:
        recalcPreGain(a);
        break;
    }
    // Low/Mid/High Solo and Bypass are read directly from v[] in step() -
    // no cached derived value needed for them.
}

// ─── Audio processing ─────────────────────────────────────────────────────────
//
//  3-way split (ported from SmoothEQ3.cpp stage 1 only):
//    midRaw = LPF(in, Mid-High Freq);  treble = in - midRaw
//    bass   = LPF(midRaw, Low-Mid Freq); mid = midRaw - bass
//  bass + mid + treble always sums back to `in` (complementary/phase-coherent
//  split), so unity band gains + ratio=0 is bit-transparent.
//
//  Per-band compressor (ported from Pop3.cpp:80-97, gate section dropped),
//  run independently per band with a Pop3-style stereo-link nudge. Threshold
//  is a real dB parameter, converted to linear in recalcBand().
//
//  Solo: if any band has Solo=ON, every non-soloed band is silenced from
//  both the main output AND its own optional output bus.
//
//  Bypass: skips the crossover/compressor entirely, passes input straight to
//  the main output (respecting Add/Replace + routing), and mutes all band
//  outputs - a true bypass rather than just freezing gain at unity.
//
//  preGain/invPreGain bridges disting NT's volt-scale busFrames into the
//  ~[-1,1] range the ported threshold math assumes.
//
void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    MultibandCompAlg*     a = (MultibandCompAlg*)self;
    MultibandCompAlg_DTC* d = a->dtc;

    const int n = numFramesBy4 * 4;

    const float* inL = busFrames + (a->v[kParamInputL] - 1) * n;
    const float* inR = busFrames + (a->v[kParamInputR] - 1) * n;

    float* outL = busFrames + (a->v[kParamOutputL] - 1) * n;
    float* outR = busFrames + (a->v[kParamOutputR] - 1) * n;
    const bool mainRepL = (a->v[kParamOutputLMode] != 0);
    const bool mainRepR = (a->v[kParamOutputRMode] != 0);

    // Band outputs: bus 0 = disconnected, skip resolving/writing entirely.
    float* lowOutL  = (a->v[kParamLowOutL]  != 0) ? busFrames + (a->v[kParamLowOutL]  - 1) * n : nullptr;
    float* lowOutR  = (a->v[kParamLowOutR]  != 0) ? busFrames + (a->v[kParamLowOutR]  - 1) * n : nullptr;
    float* midOutL  = (a->v[kParamMidOutL]  != 0) ? busFrames + (a->v[kParamMidOutL]  - 1) * n : nullptr;
    float* midOutR  = (a->v[kParamMidOutR]  != 0) ? busFrames + (a->v[kParamMidOutR]  - 1) * n : nullptr;
    float* highOutL = (a->v[kParamHighOutL] != 0) ? busFrames + (a->v[kParamHighOutL] - 1) * n : nullptr;
    float* highOutR = (a->v[kParamHighOutR] != 0) ? busFrames + (a->v[kParamHighOutR] - 1) * n : nullptr;

    const bool lowRepL  = (a->v[kParamLowOutLMode]  != 0);
    const bool lowRepR  = (a->v[kParamLowOutRMode]  != 0);
    const bool midRepL  = (a->v[kParamMidOutLMode]  != 0);
    const bool midRepR  = (a->v[kParamMidOutRMode]  != 0);
    const bool highRepL = (a->v[kParamHighOutLMode] != 0);
    const bool highRepR = (a->v[kParamHighOutRMode] != 0);

    const float meterCoeff = a->meterCoeff;

    // ── Bypass: dry passthrough, band outputs muted, no crossover/compression ──
    if (a->v[kParamBypass] != 0) {
        for (int i = 0; i < n; ++i) {
            const float passL = inL[i];
            const float passR = inR[i];
            if (mainRepL) outL[i] = passL; else outL[i] += passL;
            if (mainRepR) outR[i] = passR; else outR[i] += passR;

            if (lowOutL  && lowRepL)  lowOutL[i]  = 0.0f;
            if (lowOutR  && lowRepR)  lowOutR[i]  = 0.0f;
            if (midOutL  && midRepL)  midOutL[i]  = 0.0f;
            if (midOutR  && midRepR)  midOutR[i]  = 0.0f;
            if (highOutL && highRepL) highOutL[i] = 0.0f;
            if (highOutR && highRepR) highOutR[i] = 0.0f;

            for (int b = 0; b < kNumBands; ++b)
                a->grAvgLin[b] += (1.0f - a->grAvgLin[b]) * meterCoeff;
        }
        return;
    }

    const bool soloLow  = (a->v[kParamLowSolo]  != 0);
    const bool soloMid  = (a->v[kParamMidSolo]  != 0);
    const bool soloHigh = (a->v[kParamHighSolo] != 0);
    const bool anySolo  = soloLow || soloMid || soloHigh;
    const bool muted[kNumBands] = {
        anySolo && !soloLow,
        anySolo && !soloMid,
        anySolo && !soloHigh
    };

    // Hoist buffer-constant values
    const float xoHiA0 = a->xoHiA0, xoHiA1 = a->xoHiA1, xoHiA2 = a->xoHiA2, xoHiB1 = a->xoHiB1, xoHiB2 = a->xoHiB2;
    const float xoLoA0 = a->xoLoA0, xoLoA1 = a->xoLoA1, xoLoA2 = a->xoLoA2, xoLoB1 = a->xoLoB1, xoLoB2 = a->xoLoB2;

    float thBand[kNumBands], raBand[kNumBands], atBand[kNumBands], reBand[kNumBands], gnBand[kNumBands];
    for (int b = 0; b < kNumBands; ++b) {
        thBand[b] = a->thresh[b];
        raBand[b] = a->ratio[b];
        atBand[b] = a->attack[b];
        reBand[b] = a->release[b];
        gnBand[b] = a->gainLin[b];
    }

    const float preGain    = a->preGain;
    const float invPreGain = 1.0f / preGain;

    for (int i = 0; i < n; ++i) {
        const float rawL = inL[i] * preGain;
        const float rawR = inR[i] * preGain;

        // Mid-High crossover stage
        float outSample = (rawL * xoHiA0) + d->xoHiSL1;
        d->xoHiSL1 = (rawL * xoHiA1) - (outSample * xoHiB1) + d->xoHiSL2;
        d->xoHiSL2 = (rawL * xoHiA2) - (outSample * xoHiB2);
        const float midRawL = outSample;
        const float trebleL = rawL - midRawL;

        outSample = (rawR * xoHiA0) + d->xoHiSR1;
        d->xoHiSR1 = (rawR * xoHiA1) - (outSample * xoHiB1) + d->xoHiSR2;
        d->xoHiSR2 = (rawR * xoHiA2) - (outSample * xoHiB2);
        const float midRawR = outSample;
        const float trebleR = rawR - midRawR;

        // Low-Mid crossover stage, applied to the Mid-High stage's lowpass output
        outSample = (midRawL * xoLoA0) + d->xoLoSL1;
        d->xoLoSL1 = (midRawL * xoLoA1) - (outSample * xoLoB1) + d->xoLoSL2;
        d->xoLoSL2 = (midRawL * xoLoA2) - (outSample * xoLoB2);
        const float bassL = outSample;
        const float midL  = midRawL - bassL;

        outSample = (midRawR * xoLoA0) + d->xoLoSR1;
        d->xoLoSR1 = (midRawR * xoLoA1) - (outSample * xoLoB1) + d->xoLoSR2;
        d->xoLoSR2 = (midRawR * xoLoA2) - (outSample * xoLoB2);
        const float bassR = outSample;
        const float midR  = midRawR - bassR;

        float bandL[kNumBands] = { bassL, midL, trebleL };
        float bandR[kNumBands] = { bassR, midR, trebleR };

        float mainL = 0.0f, mainR = 0.0f;

        for (int b = 0; b < kNumBands; ++b) {
            float bL = bandL[b];
            float bR = bandR[b];
            const float absL = fabsf(bL);
            const float absR = fabsf(bR);
            float& cL = d->compL[b];
            float& cR = d->compR[b];
            const float th  = thBand[b];
            const float atk = atBand[b];
            const float rel = reBand[b];
            const float ra  = raBand[b];

            if (absL > th) { cL -= cL * atk; cL += (th / absL) * atk; }
            else            cL = cL * (1.0f - rel) + rel;
            if (absR > th) { cR -= cR * atk; cR += (th / absR) * atk; }
            else            cR = cR * (1.0f - rel) + rel;

            // Pop3-style stereo link: nudge the higher-reduction channel toward the lower
            if (cL > cR) cL -= cL * atk;
            if (cR > cL) cR -= cR * atk;

            cL = fmaxf(fminf(cL, 1.0f), 0.0f);
            cR = fmaxf(fminf(cR, 1.0f), 0.0f);

            const float gFactorL = (1.0f - ra) + cL * ra;
            const float gFactorR = (1.0f - ra) + cR * ra;

            // Meter tracks the true compression gain factor regardless of solo/mute.
            a->grAvgLin[b] += (0.5f * (gFactorL + gFactorR) - a->grAvgLin[b]) * meterCoeff;

            bL *= gFactorL * gnBand[b];
            bR *= gFactorR * gnBand[b];

            if (muted[b]) { bL = 0.0f; bR = 0.0f; }

            bandL[b] = bL;
            bandR[b] = bR;
            mainL += bL;
            mainR += bR;
        }

        mainL *= invPreGain;
        mainR *= invPreGain;

        if (mainRepL) outL[i] = mainL; else outL[i] += mainL;
        if (mainRepR) outR[i] = mainR; else outR[i] += mainR;

        if (lowOutL)  { const float v = bandL[kBandLow]  * invPreGain; if (lowRepL)  lowOutL[i]  = v; else lowOutL[i]  += v; }
        if (lowOutR)  { const float v = bandR[kBandLow]  * invPreGain; if (lowRepR)  lowOutR[i]  = v; else lowOutR[i]  += v; }
        if (midOutL)  { const float v = bandL[kBandMid]  * invPreGain; if (midRepL)  midOutL[i]  = v; else midOutL[i]  += v; }
        if (midOutR)  { const float v = bandR[kBandMid]  * invPreGain; if (midRepR)  midOutR[i]  = v; else midOutR[i]  += v; }
        if (highOutL) { const float v = bandL[kBandHigh] * invPreGain; if (highRepL) highOutL[i] = v; else highOutL[i] += v; }
        if (highOutR) { const float v = bandR[kBandHigh] * invPreGain; if (highRepR) highOutR[i] = v; else highOutR[i] += v; }
    }
}

// ─── Display ─────────────────────────────────────────────────────────────────
// 256x64, 4bpp, colours 0-15. Top 10 rows are left untouched (standard
// parameter header). The rest depicts the crossover split log-frequency-wise
// (per crossover-slopes.png: labeled crossover points dividing Low/Mid/High
// zones), with a per-band gain-reduction meter that fills downward from the
// top of its zone as reduction increases (0 dB = empty, -20 dB = full zone
// height), and the ~1s-averaged reduction printed under each zone.

static void drawDashedVLine(int x, int y0, int y1, int colour) {
    for (int y = y0; y < y1; y += 4) {
        int yEnd = y + 2;
        if (yEnd > y1) yEnd = y1;
        NT_drawShapeI(kNT_line, x, y, x, yEnd, colour);
    }
}

static int formatDb(char* buf, float db) {
    if (db > -0.5f && db < 0.0f) db = 0.0f;  // avoid printing "-0dB"
    int len = NT_floatToString(buf, db, 0);
    buf[len++] = 'd';
    buf[len++] = 'B';
    buf[len] = 0;
    return len;
}

static int formatHz(char* buf, float hz) {
    int len = NT_intToString(buf, (int32_t)(hz + 0.5f));
    buf[len++] = 'H';
    buf[len++] = 'z';
    buf[len] = 0;
    return len;
}

bool draw(_NT_algorithm* self) {
    MultibandCompAlg* a = (MultibandCompAlg*)self;

    const int x0 = 4, x1 = 252;
    const int graphW = x1 - x0;
    const int yHzLabel    = 15;
    const int yBandName   = 20;
    const int yMeterTop   = 23;
    const int yMeterBottom = 50;
    const int yDbLabel    = 61;

    int xoLowPix  = x0 + (int)(((float)a->v[kParamLowMidFreq]  / 1000.0f) * (float)graphW);
    int xoHighPix = x0 + (int)(((float)a->v[kParamMidHighFreq] / 1000.0f) * (float)graphW);
    if (xoLowPix > xoHighPix) xoLowPix = xoHighPix;  // keep zones sane if crossed

    const int zoneX[4] = { x0, xoLowPix, xoHighPix, x1 };

    const bool soloLow  = (a->v[kParamLowSolo]  != 0);
    const bool soloMid  = (a->v[kParamMidSolo]  != 0);
    const bool soloHigh = (a->v[kParamHighSolo] != 0);
    const bool anySolo  = soloLow || soloMid || soloHigh;
    const bool solo[kNumBands]  = { soloLow, soloMid, soloHigh };
    static const char* const kBandNames[kNumBands] = { "LOW", "MID", "HIGH" };

    // Dashed crossover divider lines, per crossover-slopes.png's dashed markers
    drawDashedVLine(zoneX[1], yHzLabel, yMeterBottom, 12);
    drawDashedVLine(zoneX[2], yHzLabel, yMeterBottom, 12);

    char buf[16];
    formatHz(buf, paramToFreq(a->v[kParamLowMidFreq]));
    NT_drawText(zoneX[1], yHzLabel, buf, 15, kNT_textCentre, kNT_textTiny);
    formatHz(buf, paramToFreq(a->v[kParamMidHighFreq]));
    NT_drawText(zoneX[2], yHzLabel, buf, 15, kNT_textCentre, kNT_textTiny);

    for (int b = 0; b < kNumBands; ++b) {
        const int zx0 = zoneX[b] + 1;
        const int zx1 = zoneX[b + 1] - 1;
        const int zc  = (zx0 + zx1) / 2;

        const bool bandMuted = anySolo && !solo[b];
        const int nameColour = bandMuted ? 3 : (solo[b] ? 15 : 9);
        NT_drawText(zc, yBandName, kBandNames[b], nameColour, kNT_textCentre, kNT_textTiny);

        // Gain-reduction meter: 0 dB at top (empty), -20 dB fills the full zone height
        float grDb = 20.0f * log10f(fmaxf(a->grAvgLin[b], 1e-4f));
        if (grDb > 0.0f) grDb = 0.0f;
        const float frac = fminf(-grDb / 20.0f, 1.0f);
        const int meterH = yMeterBottom - yMeterTop;
        const int fillY  = yMeterTop + (int)(frac * (float)meterH);

        NT_drawShapeI(kNT_box, zx0, yMeterTop, zx1, yMeterBottom, 3);
        if (zx1 > zx0 && fillY > yMeterTop)
            NT_drawShapeI(kNT_rectangle, zx0, yMeterTop, zx1, fillY, 6);

        formatDb(buf, grDb);
        NT_drawText(zc, yDbLabel, buf, 15, kNT_textCentre, kNT_textNormal);
    }

    if (a->v[kParamBypass] != 0)
        NT_drawText(128, 37, "BYPASSED", 15, kNT_textCentre, kNT_textNormal);

    return false;
}

// ─── Factory ───────────────────────────────────────────────────────────────────
static const _NT_factory factory = {
    .guid = NT_MULTICHAR('M', 'B', 'C', 'p'),
    .name = "MultibandComp",
    .description = "3-band compressor with independent per-band outputs",
    .numSpecifications = 0,
    .specifications = NULL,
    .calculateStaticRequirements = NULL,
    .initialise = NULL,
    .calculateRequirements = calculateRequirements,
    .construct = construct,
    .parameterChanged = parameterChanged,
    .step = step,
    .draw = draw,
    .midiRealtime = NULL,
    .midiMessage = NULL,
    .tags = kNT_tagEffect,
    .hasCustomUi = NULL,
    .customUi = NULL,
    .setupUi = NULL,
    .serialise = NULL,
    .deserialise = NULL,
    .midiSysEx = NULL,
    .parameterUiPrefix = NULL,
    .parameterString = NULL,
};

extern "C" uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
    switch (selector) {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return (uintptr_t)(data == 0 ? &factory : NULL);
    }
    return 0;
}
