#include "distingnt/api.h"
#include <cmath>
#include <cstring>
#include <new>

// ─── Constants ───────────────────────────────────────────────────────────────
static const float kPi   = 3.14159265358979f;
static const float kAP_R = 0.9f;

static const char* const kItoONames[]      = { "Mix", "Exciter", NULL };
static const char* const kWaveNames[]      = { "Sine", "Saw", "Square", "Noise", NULL };
static const char* const kHoldTrigNames[]  = { "OFF", "ON", NULL };

// 4th-order Butterworth pole-pair Qs
static const float kBwQ[2] = { 1.3066f, 0.5412f };

// Exciter wavetable: 3 × 1024 floats in shared DRAM
static const int   kWT = 1024;
static float*      g_excWT = nullptr;

// ─── DTC ─────────────────────────────────────────────────────────────────────
struct DetunePhaserAlg_DTC {
    // Phaser: AP delay lines
    float ap_x1[24], ap_x2[24];
    float ap_y1[24], ap_y2[24];
    float prev_out;
    // Phaser: HPF / LPF delay lines
    float h_x1[2], h_x2[2], h_y1[2], h_y2[2];
    float l_x1[2], l_x2[2], l_y1[2], l_y2[2];
    // Exciter
    float    excPhase;
    float    excEnvVal;
    float    excPrevIn;
    int32_t  excState;
    int32_t  excRemain;
    uint32_t noiseState;
};

// ─── Algorithm struct ────────────────────────────────────────────────────────
struct DetunePhaserAlg : public _NT_algorithm {
    DetunePhaserAlg_DTC* dtc;

    // All-pass biquad coefficients
    float ap_c1s[24], ap_c2;
    float fb;

    // Glide state: cur_theta interpolates toward tgt_theta at glideRate rad/sample
    float cur_theta, tgt_theta;
    float glideRate;   // 0 = instant

    // HPF biquad:  y = b0·(x − 2x1 + x2) − a1·y1 − a2·y2
    float hpf_b0[2], hpf_a1[2], hpf_a2[2];
    // LPF biquad:  y = b0·(x + 2x1 + x2) − a1·y1 − a2·y2
    float lpf_b0[2], lpf_a1[2], lpf_a2[2];

    // Exciter
    float   excPhaseInc;
    float   excAttackCoeff;
    float   excReleaseCoeff;
    int32_t excAttackSamples;
    int32_t excHoldSamples;

    // %RANDOM PRNG state (xorshift32)
    uint32_t randState;

    DetunePhaserAlg(DetunePhaserAlg_DTC* d)
        : dtc(d),
          ap_c2(kAP_R * kAP_R),
          fb(0.0f),
          cur_theta(0.0f), tgt_theta(0.0f),
          glideRate(0.0f),
          excPhaseInc(200.0f / 48000.0f),
          excAttackCoeff(0.01f), excReleaseCoeff(0.999f),
          excAttackSamples(480), excHoldSamples(4800),
          randState(0xA5A5A5A5u)
    {
        for (int s = 0; s < 24; ++s) ap_c1s[s] = 0.0f;
    }
};

// ─── Parameters ──────────────────────────────────────────────────────────────
enum {
    kParamInput,
    kParamOutput,
    kParamOutputMode,
    kParamHPF,            // 0-1000 → 20 Hz–17 kHz log
    kParamLPF,            // 0-1000 → 20 Hz–17 kHz log
    kParamPhase,           // 0-1000 → pole angle 0–π/2
    kParamFeedback,        // 0-2000 → 0.0–99.9% feedback
    kParamDetune,          // 0-3600 → 0.0°–360.0° per-stage spread (tenths of degree)
    kParamGlide,           // 0-5000 ms → linear phase glide time
    kParamHoldTrigPhase,   // 0=OFF  1=ON (Exciter mode only)
    kParamInputToOutput,   // 0=Mix  1=Exciter
    kParamExciterFreq,     // 5–2000 Hz
    kParamExciterWave,     // 0=Sine  1=Saw  2=Square
    kParamAttack,          // 1–2000 ms
    kParamHold,            // 1–2000 ms
    kParamRelease,         // 1–2000 ms
    kParamRandFire,        // 0=OFF  1=ON → OFF→ON transition fires a new randomisation
    kParamRandPercent,     // 0-100 → max % of full range to randomise Phase/Detune by
    kNumParams
};

static const _NT_parameter parameters[] = {
    NT_PARAMETER_AUDIO_INPUT("Input", 1, 1)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Output", 1, 13)
    { .name="HPF",           .min=0,   .max=1000, .def=0,
      .unit=kNT_unitNone,    .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="LPF",           .min=0,   .max=1000, .def=1000,
      .unit=kNT_unitNone,    .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Phase",         .min=0,   .max=1000, .def=500,
      .unit=kNT_unitNone,    .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Feedback",      .min=0,   .max=2000, .def=0,
      .unit=kNT_unitPercent, .scaling=kNT_scaling10,   .enumStrings=NULL },
    { .name="Detune",        .min=0,   .max=3600, .def=0,
      .unit=kNT_unitDegrees, .scaling=kNT_scaling10,   .enumStrings=NULL },
    { .name="Glide",         .min=0,   .max=5000, .def=0,
      .unit=kNT_unitMs,      .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="HoldTrigPhase", .min=0,   .max=1,    .def=0,
      .unit=kNT_unitEnum,    .scaling=kNT_scalingNone, .enumStrings=kHoldTrigNames },
    { .name="InputToOutput", .min=0,   .max=1,    .def=0,
      .unit=kNT_unitEnum,    .scaling=kNT_scalingNone, .enumStrings=kItoONames },
    { .name="ExciterFreq",   .min=5,   .max=2000, .def=200,
      .unit=kNT_unitHz,      .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="ExciterWave",   .min=0,   .max=3,    .def=0,
      .unit=kNT_unitEnum,    .scaling=kNT_scalingNone, .enumStrings=kWaveNames },
    { .name="Attack",        .min=1,   .max=2000, .def=10,
      .unit=kNT_unitMs,      .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Hold",          .min=1,   .max=2000, .def=100,
      .unit=kNT_unitMs,      .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Release",       .min=1,   .max=2000, .def=500,
      .unit=kNT_unitMs,      .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="FiRE",          .min=0,   .max=1,    .def=0,
      .unit=kNT_unitEnum,    .scaling=kNT_scalingNone, .enumStrings=kHoldTrigNames },
    { .name="Percent",       .min=0,   .max=100,  .def=0,
      .unit=kNT_unitPercent, .scaling=kNT_scalingNone, .enumStrings=NULL },
};

// ─── Pages ───────────────────────────────────────────────────────────────────
static const uint8_t phaserPageParams[] = {
    kParamInputToOutput,
    kParamHPF, kParamLPF,
    kParamPhase, kParamFeedback,
    kParamDetune, kParamGlide, kParamHoldTrigPhase
};
static const uint8_t exciterPageParams[] = {
    kParamExciterFreq, kParamExciterWave,
    kParamAttack, kParamHold, kParamRelease
};
static const uint8_t routingPageParams[] = {
    kParamInput, kParamOutput, kParamOutputMode
};
static const uint8_t randomPageParams[] = {
    kParamRandFire, kParamRandPercent
};
static const _NT_parameterPage pages[] = {
    { .name="Phaser",   .numParams=ARRAY_SIZE(phaserPageParams),  .group=0, .params=phaserPageParams },
    { .name="Exciter",  .numParams=ARRAY_SIZE(exciterPageParams), .group=0, .params=exciterPageParams },
    { .name="Routing",  .numParams=ARRAY_SIZE(routingPageParams), .group=0, .params=routingPageParams },
    { .name="%RANDOM",  .numParams=ARRAY_SIZE(randomPageParams),  .group=0, .params=randomPageParams },
};
static const _NT_parameterPages paramPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages    = pages,
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

static inline float paramToFreq(int v) {
    return 20.0f * powf(850.0f, (float)v / 1000.0f);
}

static inline void bwFeedback(float K, float K2, int s, float* a1, float* a2) {
    const float norm = 1.0f + K / kBwQ[s] + K2;
    *a1 = 2.0f * (K2 - 1.0f) / norm;
    *a2 = (1.0f - K / kBwQ[s] + K2) / norm;
}

static void recalcHPF(DetunePhaserAlg* a) {
    const float K  = tanf(kPi * paramToFreq(a->v[kParamHPF]) / NT_globals.sampleRate);
    const float K2 = K * K;
    for (int s = 0; s < 2; ++s) {
        const float norm = 1.0f + K / kBwQ[s] + K2;
        a->hpf_b0[s] = 1.0f / norm;
        bwFeedback(K, K2, s, &a->hpf_a1[s], &a->hpf_a2[s]);
    }
}

static void recalcLPF(DetunePhaserAlg* a) {
    const float K  = tanf(kPi * paramToFreq(a->v[kParamLPF]) / NT_globals.sampleRate);
    const float K2 = K * K;
    for (int s = 0; s < 2; ++s) {
        const float norm = 1.0f + K / kBwQ[s] + K2;
        a->lpf_b0[s] = K2 / norm;
        bwFeedback(K, K2, s, &a->lpf_a1[s], &a->lpf_a2[s]);
    }
}

// Recompute c1s from cur_theta + current detune (used during glide and on detune change)
static void recalcC1s(DetunePhaserAlg* a) {
    const float delta = (float)a->v[kParamDetune] * kPi / 18000.0f;
    for (int s = 0; s < 24; ++s)
        a->ap_c1s[s] = 2.0f * kAP_R * cosf(a->cur_theta + (float)(s - 11) * delta);
}

// Update tgt_theta and fb from v[].
// If glide is off (glideRate==0), snap cur_theta and recalc c1s immediately.
static void recalcPhaser(DetunePhaserAlg* a) {
    a->ap_c2     = kAP_R * kAP_R;
    a->fb        = (float)a->v[kParamFeedback] * 0.999f / 2000.0f;
    a->tgt_theta = (float)a->v[kParamPhase] * kPi / 2000.0f;
    if (a->glideRate == 0.0f) { a->cur_theta = a->tgt_theta; recalcC1s(a); }
}

// Recompute glideRate from v[kParamGlide]; snap cur to tgt if glide is turned off
static void recalcGlide(DetunePhaserAlg* a) {
    const int v = a->v[kParamGlide];
    if (v == 0) {
        a->glideRate = 0.0f;
        a->cur_theta = a->tgt_theta; recalcC1s(a);
    } else {
        // Full θ range (π/2) traversed in v milliseconds
        a->glideRate = (kPi * 0.5f) / ((float)v * 0.001f * NT_globals.sampleRate);
    }
}

static void recalcExciter(DetunePhaserAlg* a, int p) {
    const float sr = NT_globals.sampleRate;
    switch (p) {
    case kParamExciterFreq:
        a->excPhaseInc = (float)a->v[kParamExciterFreq] / sr;
        break;
    case kParamAttack: {
        const float samps = (float)a->v[kParamAttack] * 0.001f * sr;
        a->excAttackSamples = (int32_t)(samps + 0.5f);
        a->excAttackCoeff   = 1.0f - powf(0.001f, 1.0f / samps);
        break;
    }
    case kParamHold:
        a->excHoldSamples = (int32_t)((float)a->v[kParamHold] * 0.001f * sr + 0.5f);
        break;
    case kParamRelease: {
        const float samps = (float)a->v[kParamRelease] * 0.001f * sr;
        a->excReleaseCoeff = powf(0.001f, 1.0f / samps);
        break;
    }
    }
}

static inline float nextRandBipolar(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return (float)(int32_t)state * (1.0f / 2147483648.0f);  // [-1, 1)
}

// Randomise Phase and Detune by up to ±(Percent% of each parameter's full range),
// applied on top of their currently set values.
static void doRandomize(DetunePhaserAlg* a) {
    static const int kRandParams[2] = { kParamPhase, kParamDetune };
    const float   pct = (float)a->v[kParamRandPercent] * 0.01f;
    const int32_t idx = NT_algorithmIndex(a);

    for (int i = 0; i < 2; ++i) {
        const int p = kRandParams[i];
        const _NT_parameter& info = parameters[p];
        const float range  = (float)(info.max - info.min);
        const float maxAdj = range * pct;
        const float r      = nextRandBipolar(a->randState);

        int32_t newVal = (int32_t)a->v[p] + (int32_t)(r * maxAdj);
        if (newVal < info.min) newVal = info.min;
        if (newVal > info.max) newVal = info.max;

        NT_setParameterFromAudio(idx, p + NT_parameterOffset(), (int16_t)newVal);
    }
}

// ─── Static memory ────────────────────────────────────────────────────────────

void calculateStaticRequirements(_NT_staticRequirements& req) {
    req.dram = 3 * kWT * (int)sizeof(float);
}

void initialise(_NT_staticMemoryPtrs& ptrs, const _NT_staticRequirements&) {
    g_excWT = (float*)ptrs.dram;
    for (int i = 0; i < kWT; ++i)
        g_excWT[i] = sinf(2.0f * kPi * (float)i / (float)kWT);
    for (int i = 0; i < kWT; ++i)
        g_excWT[kWT + i] = 2.0f * (float)i / (float)kWT - 1.0f;
    for (int i = 0; i < kWT; ++i)
        g_excWT[2 * kWT + i] = (i < kWT / 2) ? 1.0f : -1.0f;
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t*) {
    req.numParameters = ARRAY_SIZE(parameters);
    req.sram = sizeof(DetunePhaserAlg);
    req.dram = 0;
    req.dtc  = sizeof(DetunePhaserAlg_DTC);
    req.itc  = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs,
                          const _NT_algorithmRequirements&,
                          const int32_t*) {
    DetunePhaserAlg_DTC* d = (DetunePhaserAlg_DTC*)ptrs.dtc;
    memset(d, 0, sizeof(*d));
    d->noiseState = 0x12345678u;

    DetunePhaserAlg* a = new (ptrs.sram) DetunePhaserAlg(d);
    a->parameters     = parameters;
    a->parameterPages = &paramPages;

    // Glide initialisation: default glide = 0 (instant), snap cur to default theta
    a->glideRate = 0.0f;
    a->tgt_theta = (float)a->v[kParamPhase] * kPi / 2000.0f;
    a->cur_theta = a->tgt_theta;

    recalcPhaser(a);
    recalcHPF(a);
    recalcLPF(a);
    recalcExciter(a, kParamExciterFreq);
    recalcExciter(a, kParamAttack);
    recalcExciter(a, kParamHold);
    recalcExciter(a, kParamRelease);

    // HoldTrigPhase only valid in Exciter mode; gray out at startup (default=Mix)
    {
        const int32_t idx = NT_algorithmIndex(a);
        NT_setParameterGrayedOut(idx, kParamHoldTrigPhase + NT_parameterOffset(), true);
    }

    return a;
}

void parameterChanged(_NT_algorithm* self, int p) {
    DetunePhaserAlg* a = (DetunePhaserAlg*)self;
    const bool excMode  = (a->v[kParamInputToOutput] == 1);
    const bool holdTrig = excMode && (a->v[kParamHoldTrigPhase] == 1);
    switch (p) {
    case kParamHPF:     recalcHPF(a); break;
    case kParamLPF:     recalcLPF(a); break;
    case kParamPhase:
        if (!holdTrig) {
            a->tgt_theta = (float)a->v[kParamPhase] * kPi / 2000.0f;
            if (a->glideRate == 0.0f) { a->cur_theta = a->tgt_theta; recalcC1s(a); }
        }
        break;
    case kParamFeedback:
        if (!holdTrig) a->fb = (float)a->v[kParamFeedback] * 0.999f / 2000.0f;
        break;
    case kParamDetune:
        if (!holdTrig) recalcC1s(a);
        break;
    case kParamGlide:
        recalcGlide(a);
        break;
    case kParamHoldTrigPhase:
        // Turning OFF: flush all deferred values immediately
        if (a->v[kParamHoldTrigPhase] == 0) {
            a->tgt_theta = (float)a->v[kParamPhase] * kPi / 2000.0f;
            a->fb = (float)a->v[kParamFeedback] * 0.999f / 2000.0f;
            if (a->glideRate == 0.0f) { a->cur_theta = a->tgt_theta; recalcC1s(a); }
        }
        break;
    case kParamInputToOutput: {
        const int32_t idx = NT_algorithmIndex(self);
        NT_setParameterGrayedOut(idx, kParamHoldTrigPhase + NT_parameterOffset(), !excMode);
        // Leaving Exciter mode: apply any deferred values
        if (!excMode) {
            a->tgt_theta = (float)a->v[kParamPhase] * kPi / 2000.0f;
            a->fb = (float)a->v[kParamFeedback] * 0.999f / 2000.0f;
            if (a->glideRate == 0.0f) { a->cur_theta = a->tgt_theta; recalcC1s(a); }
        }
        break;
    }
    case kParamExciterFreq:
    case kParamAttack:
    case kParamHold:
    case kParamRelease: recalcExciter(a, p); break;
    case kParamRandFire:
        if (a->v[kParamRandFire] != 0) doRandomize(a);
        break;
    }
}

// ─── Audio processing ─────────────────────────────────────────────────────────
//
//  Mix mode:     chainIn = xin;  pout = 0.5·xin + 0.5·AP_out
//
//  Exciter mode: chainIn = oscillator × AHD envelope (±10V peak)
//                pout = AP_out
//
//  Feedback summer: sig = chainIn · (1 − 0.9·f) + prev_out · f
//    At f=0.999 (max): chainIn retains 10%; at f=0: full chainIn passes through.
//
//  Glide: cur_theta advances toward tgt_theta at glideRate rad/sample (buffer-rate).
//  Output: hard ±10V clamp on bus write.
//
void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    DetunePhaserAlg*     a = (DetunePhaserAlg*)self;
    DetunePhaserAlg_DTC* d = a->dtc;

    const int    n   = numFramesBy4 * 4;
    const float* in  = busFrames + (a->v[kParamInput]  - 1) * n;
    float*       out = busFrames + (a->v[kParamOutput] - 1) * n;
    const bool   rep = (a->v[kParamOutputMode] != 0);

    // ── Glide: advance cur_theta toward tgt_theta (buffer-rate) ──────────
    {
        const float advance = a->glideRate * (float)n;
        if (advance > 0.0f) {
            const float dtheta = a->tgt_theta - a->cur_theta;
            if      (dtheta >  advance) { a->cur_theta += advance;      recalcC1s(a); }
            else if (dtheta < -advance) { a->cur_theta -= advance;      recalcC1s(a); }
            else if (dtheta != 0.0f)    { a->cur_theta = a->tgt_theta;  recalcC1s(a); }
        }
    }

    // ── Hoist buffer-constant values ──────────────────────────────────────
    float c1_s[24];
    for (int s = 0; s < 24; ++s) c1_s[s] = a->ap_c1s[s];
    const float c2 = a->ap_c2, f = a->fb;
    // Input gain: 1 − 0.9·f → 10% at max feedback, 100% at zero feedback
    const float inGain = 1.0f - 0.9f * f;
    const float hb0  = a->hpf_b0[0], ha1_0 = a->hpf_a1[0], ha2_0 = a->hpf_a2[0];
    const float hb1  = a->hpf_b0[1], ha1_1 = a->hpf_a1[1], ha2_1 = a->hpf_a2[1];
    const float lb0  = a->lpf_b0[0], la1_0 = a->lpf_a1[0], la2_0 = a->lpf_a2[0];
    const float lb1  = a->lpf_b0[1], la1_1 = a->lpf_a1[1], la2_1 = a->lpf_a2[1];

    const bool    excMode       = (a->v[kParamInputToOutput] == 1);
    const bool    holdTrigPhase = excMode && (a->v[kParamHoldTrigPhase] == 1);
    const int     excWave    = a->v[kParamExciterWave];
    const float   excPInc    = a->excPhaseInc;
    const float   excAtkC    = a->excAttackCoeff;
    const float   excRelC    = a->excReleaseCoeff;
    const int32_t excAtkS    = a->excAttackSamples;
    const int32_t excHldS    = a->excHoldSamples;

    for (int i = 0; i < n; ++i) {
        const float xin = in[i];

        // ── Exciter ───────────────────────────────────────────────────────
        float excSig = 0.0f;
        if (excMode) {
            if (xin >= 1.0f && d->excPrevIn < 1.0f) {
                d->excPhase  = 0.0f;
                d->excEnvVal = 0.0f;
                d->excState  = 1;
                d->excRemain = excAtkS;
                // HoldTrigPhase: latch deferred changes, kick off glide
                if (holdTrigPhase) {
                    a->tgt_theta = (float)a->v[kParamPhase] * kPi / 2000.0f;
                    a->fb = (float)a->v[kParamFeedback] * 0.999f / 2000.0f;
                    if (a->glideRate == 0.0f) {
                        a->cur_theta = a->tgt_theta; recalcC1s(a);
                        // Reload hoisted c1 array so new coefficients take effect this buffer
                        for (int ss = 0; ss < 24; ++ss) c1_s[ss] = a->ap_c1s[ss];
                    }
                }
            }
            d->excPrevIn = xin;

            float env = d->excEnvVal;
            switch (d->excState) {
            case 1: // Attack
                env += (10.0f - env) * excAtkC;
                if (--d->excRemain <= 0) {
                    env = 10.0f;
                    d->excState  = 2;
                    d->excRemain = excHldS;
                }
                break;
            case 2: // Hold
                env = 10.0f;
                if (--d->excRemain <= 0) d->excState = 3;
                break;
            case 3: // Release
                env *= excRelC;
                if (env < 0.001f) { env = 0.0f; d->excState = 0; }
                break;
            default: env = 0.0f; break;
            }
            d->excEnvVal = env;

            if (env > 0.0f) {
                if (excWave == 3) {
                    // White noise: xorshift32
                    uint32_t s32 = d->noiseState;
                    s32 ^= s32 << 13;
                    s32 ^= s32 >> 17;
                    s32 ^= s32 << 5;
                    d->noiseState = s32;
                    // Map uint32 to [-1, 1)
                    excSig = ((float)(int32_t)s32 * (1.0f / 2147483648.0f)) * env;
                } else {
                    const float* tbl = g_excWT + excWave * kWT;
                    const float  pos = d->excPhase * (float)kWT;
                    const int    i0  = (int)pos & (kWT - 1);
                    const float  t   = pos - (float)i0;
                    excSig = (tbl[i0] + t * (tbl[(i0 + 1) & (kWT - 1)] - tbl[i0])) * env;
                    d->excPhase += excPInc;
                    if (d->excPhase >= 1.0f) d->excPhase -= 1.0f;
                }
            }
        }

        const float chainIn = excMode ? excSig : xin;

        // ── Phaser ────────────────────────────────────────────────────────
        float sig = chainIn * inGain + d->prev_out * f;

        // HPF — 4th-order Butterworth
        {
            float hx = sig;
            float hy = hb0*(hx - 2.0f*d->h_x1[0] + d->h_x2[0])
                       - ha1_0*d->h_y1[0] - ha2_0*d->h_y2[0];
            d->h_x2[0]=d->h_x1[0]; d->h_x1[0]=hx;
            d->h_y2[0]=d->h_y1[0]; d->h_y1[0]=hy; hx=hy;
            hy = hb1*(hx - 2.0f*d->h_x1[1] + d->h_x2[1])
                 - ha1_1*d->h_y1[1] - ha2_1*d->h_y2[1];
            d->h_x2[1]=d->h_x1[1]; d->h_x1[1]=hx;
            d->h_y2[1]=d->h_y1[1]; d->h_y1[1]=hy; sig=hy;
        }
        // LPF — 4th-order Butterworth
        {
            float lx = sig;
            float ly = lb0*(lx + 2.0f*d->l_x1[0] + d->l_x2[0])
                       - la1_0*d->l_y1[0] - la2_0*d->l_y2[0];
            d->l_x2[0]=d->l_x1[0]; d->l_x1[0]=lx;
            d->l_y2[0]=d->l_y1[0]; d->l_y1[0]=ly; lx=ly;
            ly = lb1*(lx + 2.0f*d->l_x1[1] + d->l_x2[1])
                 - la1_1*d->l_y1[1] - la2_1*d->l_y2[1];
            d->l_x2[1]=d->l_x1[1]; d->l_x1[1]=lx;
            d->l_y2[1]=d->l_y1[1]; d->l_y1[1]=ly; sig=ly;
        }
        // 24× second-order all-pass stages
        for (int s = 0; s < 24; ++s) {
            const float c1 = c1_s[s];
            const float x1=d->ap_x1[s], x2=d->ap_x2[s];
            const float y1=d->ap_y1[s], y2=d->ap_y2[s];
            const float y = c2*sig - c1*x1 + x2 + c1*y1 - c2*y2;
            d->ap_x2[s]=x1; d->ap_x1[s]=sig;
            d->ap_y2[s]=y1; d->ap_y1[s]=y;
            sig=y;
        }
        const float pout = excMode ? sig : (0.5f * xin + 0.5f * sig);
        d->prev_out = pout;

        const float clamped = pout > 10.0f ? 10.0f : pout < -10.0f ? -10.0f : pout;
        out[i] = rep ? clamped : out[i] + clamped;
    }
}

// ─── Factory ──────────────────────────────────────────────────────────────────
static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('D','t','P','h'),
    .name                        = "DetunePhaser",
    .description                 = "Single 24-stage all-pass phaser with detune spread and triggered exciter",
    .numSpecifications           = 0,
    .specifications              = NULL,
    .calculateStaticRequirements = calculateStaticRequirements,
    .initialise                  = initialise,
    .calculateRequirements       = calculateRequirements,
    .construct                   = construct,
    .parameterChanged            = parameterChanged,
    .step                        = step,
    .draw                        = NULL,
    .midiRealtime                = NULL,
    .midiMessage                 = NULL,
    .tags                        = kNT_tagEffect,
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
