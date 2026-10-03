#include "distingnt/api.h"
#include <cmath>
#include <cstring>
#include <new>

// ─── Constants ───────────────────────────────────────────────────────────────
static const float kPi   = 3.14159265358979f;
static const float kAP_R = 0.9f;

static const char* const kItoONames[]      = { "Mix", "Exciter", NULL };
static const char* const kWaveNames[]      = { "Sine", "Saw", "Square", NULL };
static const char* const kHoldTrigNames[]  = { "OFF", "ON", NULL };

// 4th-order Butterworth pole-pair Qs
static const float kBwQ[2] = { 1.3066f, 0.5412f };

// Exciter wavetable: 3 × 1024 floats in shared DRAM
static const int   kWT = 1024;
static float*      g_excWT = nullptr;

// ─── DTC ─────────────────────────────────────────────────────────────────────
struct DualPhaserAlg_DTC {
    // Phaser 1: AP delay lines
    float ap1_x1[24], ap1_x2[24];
    float ap1_y1[24], ap1_y2[24];
    float prev_out1;
    // Phaser 1: HPF / LPF delay lines
    float h1_x1[2], h1_x2[2], h1_y1[2], h1_y2[2];
    float l1_x1[2], l1_x2[2], l1_y1[2], l1_y2[2];
    // Phaser 2: AP delay lines
    float ap2_x1[24], ap2_x2[24];
    float ap2_y1[24], ap2_y2[24];
    float prev_out2;
    // Phaser 2: HPF / LPF delay lines
    float h2_x1[2], h2_x2[2], h2_y1[2], h2_y2[2];
    float l2_x1[2], l2_x2[2], l2_y1[2], l2_y2[2];
    // Exciter
    float    excPhase;    // oscillator phase [0, 1)
    float    excEnvVal;   // current envelope value [0, 10]
    float    excPrevIn;   // previous input sample (rising-edge detection)
    int32_t  excState;    // 0=idle  1=attack  2=hold  3=release
    int32_t  excRemain;   // sample countdown for current segment
};

// ─── Algorithm struct ────────────────────────────────────────────────────────
struct DualPhaserAlg : public _NT_algorithm {
    DualPhaserAlg_DTC* dtc;

    // All-pass biquad coefficients (per phaser)
    float ap1_c1, ap1_c2;
    float ap2_c1, ap2_c2;
    float fb1, fb2;

    // HPF biquad:  y = b0·(x − 2x1 + x2) − a1·y1 − a2·y2
    float hpf_b0[2], hpf_a1[2], hpf_a2[2];
    // LPF biquad:  y = b0·(x + 2x1 + x2) − a1·y1 − a2·y2
    float lpf_b0[2], lpf_a1[2], lpf_a2[2];

    // Exciter (cached from parameters + sample rate)
    float   excPhaseInc;       // freq / sr
    float   excAttackCoeff;    // per-sample one-pole toward 10V
    float   excReleaseCoeff;   // per-sample multiplicative decay
    int32_t excAttackSamples;
    int32_t excHoldSamples;

    DualPhaserAlg(DualPhaserAlg_DTC* d)
        : dtc(d),
          ap1_c1(0.0f), ap1_c2(kAP_R * kAP_R),
          ap2_c1(0.0f), ap2_c2(kAP_R * kAP_R),
          fb1(0.0f), fb2(0.0f),
          excPhaseInc(200.0f / 48000.0f),
          excAttackCoeff(0.01f), excReleaseCoeff(0.999f),
          excAttackSamples(480), excHoldSamples(4800) {}
};

// ─── Parameters ──────────────────────────────────────────────────────────────
enum {
    kParamInput,
    kParamOutput,
    kParamOutputMode,
    kParamHPF,            // 0-1000 → 20 Hz–17 kHz log
    kParamLPF,            // 0-1000 → 20 Hz–17 kHz log
    kParamPhase1,         // 0-1000 → pole angle 0–π/2
    kParamFeedback1,      // 0-2000 → 0.0–200.0%
    kParamPhase2,
    kParamFeedback2,
    kParamHoldTrigPhase,  // 0=OFF  1=ON (Exciter mode only)
    kParamInputToOutput,  // 0=Mix  1=Exciter
    kParamExciterFreq,    // 5–2000 Hz
    kParamExciterWave,    // 0=Sine  1=Saw  2=Square
    kParamAttack,         // 1–2000 ms
    kParamHold,           // 1–2000 ms
    kParamRelease,        // 1–2000 ms
    kNumParams
};

static const _NT_parameter parameters[] = {
    NT_PARAMETER_AUDIO_INPUT("Input", 1, 1)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Output", 1, 13)
    { .name="HPF",           .min=0,   .max=1000, .def=0,
      .unit=kNT_unitNone,    .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="LPF",           .min=0,   .max=1000, .def=1000,
      .unit=kNT_unitNone,    .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Phase 1",       .min=0,   .max=1000, .def=500,
      .unit=kNT_unitNone,    .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Feedback 1",    .min=0,   .max=2000, .def=0,
      .unit=kNT_unitPercent, .scaling=kNT_scaling10,   .enumStrings=NULL },
    { .name="Phase 2",       .min=0,   .max=1000, .def=500,
      .unit=kNT_unitNone,    .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Feedback 2",    .min=0,   .max=2000, .def=0,
      .unit=kNT_unitPercent, .scaling=kNT_scaling10,   .enumStrings=NULL },
    { .name="HoldTrigPhase", .min=0,   .max=1,    .def=0,
      .unit=kNT_unitEnum,    .scaling=kNT_scalingNone, .enumStrings=kHoldTrigNames },
    { .name="InputToOutput", .min=0,   .max=1,    .def=0,
      .unit=kNT_unitEnum,    .scaling=kNT_scalingNone, .enumStrings=kItoONames },
    { .name="ExciterFreq",   .min=5,   .max=2000, .def=200,
      .unit=kNT_unitHz,      .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="ExciterWave",   .min=0,   .max=2,    .def=0,
      .unit=kNT_unitEnum,    .scaling=kNT_scalingNone, .enumStrings=kWaveNames },
    { .name="Attack",        .min=1,   .max=2000, .def=10,
      .unit=kNT_unitMs,      .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Hold",          .min=1,   .max=2000, .def=100,
      .unit=kNT_unitMs,      .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Release",       .min=1,   .max=2000, .def=500,
      .unit=kNT_unitMs,      .scaling=kNT_scalingNone, .enumStrings=NULL },
};

// ─── Pages ───────────────────────────────────────────────────────────────────
static const uint8_t phasersPageParams[] = {
    kParamInputToOutput,
    kParamHPF, kParamLPF,
    kParamPhase1, kParamFeedback1,
    kParamPhase2, kParamFeedback2,
    kParamHoldTrigPhase
};
static const uint8_t exciterPageParams[] = {
    kParamExciterFreq, kParamExciterWave,
    kParamAttack, kParamHold, kParamRelease
};
static const uint8_t routingPageParams[] = {
    kParamInput, kParamOutput, kParamOutputMode
};
static const _NT_parameterPage pages[] = {
    { .name="Phasers", .numParams=ARRAY_SIZE(phasersPageParams), .group=0, .params=phasersPageParams },
    { .name="Exciter", .numParams=ARRAY_SIZE(exciterPageParams), .group=0, .params=exciterPageParams },
    { .name="Routing", .numParams=ARRAY_SIZE(routingPageParams), .group=0, .params=routingPageParams },
};
static const _NT_parameterPages paramPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages    = pages,
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Log mapping: 0 → 20 Hz, 1000 → 17000 Hz
static inline float paramToFreq(int v) {
    return 20.0f * powf(850.0f, (float)v / 1000.0f);
}

static inline void bwFeedback(float K, float K2, int s, float* a1, float* a2) {
    const float norm = 1.0f + K / kBwQ[s] + K2;
    *a1 = 2.0f * (K2 - 1.0f) / norm;
    *a2 = (1.0f - K / kBwQ[s] + K2) / norm;
}

static void recalcHPF(DualPhaserAlg* a) {
    const float K  = tanf(kPi * paramToFreq(a->v[kParamHPF]) / NT_globals.sampleRate);
    const float K2 = K * K;
    for (int s = 0; s < 2; ++s) {
        const float norm = 1.0f + K / kBwQ[s] + K2;
        a->hpf_b0[s] = 1.0f / norm;
        bwFeedback(K, K2, s, &a->hpf_a1[s], &a->hpf_a2[s]);
    }
}

static void recalcLPF(DualPhaserAlg* a) {
    const float K  = tanf(kPi * paramToFreq(a->v[kParamLPF]) / NT_globals.sampleRate);
    const float K2 = K * K;
    for (int s = 0; s < 2; ++s) {
        const float norm = 1.0f + K / kBwQ[s] + K2;
        a->lpf_b0[s] = K2 / norm;
        bwFeedback(K, K2, s, &a->lpf_a1[s], &a->lpf_a2[s]);
    }
}

static void recalcPhaser(DualPhaserAlg* a, int which) {
    if (which == 0) {
        const float theta = (float)a->v[kParamPhase1]    * kPi / 2000.0f;
        a->ap1_c1         = 2.0f * kAP_R * cosf(theta);
        a->ap1_c2         = kAP_R * kAP_R;
        a->fb1            = (float)a->v[kParamFeedback1] / 1000.0f;
    } else {
        const float theta = (float)a->v[kParamPhase2]    * kPi / 2000.0f;
        a->ap2_c1         = 2.0f * kAP_R * cosf(theta);
        a->ap2_c2         = kAP_R * kAP_R;
        a->fb2            = (float)a->v[kParamFeedback2] / 1000.0f;
    }
}

static void recalcExciter(DualPhaserAlg* a, int p) {
    const float sr = NT_globals.sampleRate;
    switch (p) {
    case kParamExciterFreq:
        a->excPhaseInc = (float)a->v[kParamExciterFreq] / sr;
        break;
    case kParamAttack: {
        // Exponential attack: use one-pole toward 10V, reaching ~99.9% in attackTime.
        // coeff = 1 - (0.001)^(1/samps)  so at n=samps: err = (0.001)^1 = 0.001 of gap remaining.
        const float samps = (float)a->v[kParamAttack] * 0.001f * sr;
        a->excAttackSamples = (int32_t)(samps + 0.5f);
        a->excAttackCoeff   = 1.0f - powf(0.001f, 1.0f / samps);
        break;
    }
    case kParamHold:
        a->excHoldSamples = (int32_t)((float)a->v[kParamHold] * 0.001f * sr + 0.5f);
        break;
    case kParamRelease: {
        // Multiplicative decay: env *= releaseCoeff each sample.
        // At n=samps: env = 10 * (0.001)^1 = 0.01V → below threshold.
        const float samps = (float)a->v[kParamRelease] * 0.001f * sr;
        a->excReleaseCoeff = powf(0.001f, 1.0f / samps);
        break;
    }
    }
}

// ─── Static memory ────────────────────────────────────────────────────────────

void calculateStaticRequirements(_NT_staticRequirements& req) {
    req.dram = 3 * kWT * (int)sizeof(float);
}

void initialise(_NT_staticMemoryPtrs& ptrs, const _NT_staticRequirements&) {
    g_excWT = (float*)ptrs.dram;

    // Sine — ±1
    for (int i = 0; i < kWT; ++i)
        g_excWT[i] = sinf(2.0f * kPi * (float)i / (float)kWT);

    // Saw — rises from −1 to +1
    for (int i = 0; i < kWT; ++i)
        g_excWT[kWT + i] = 2.0f * (float)i / (float)kWT - 1.0f;

    // Square — +1 first half, −1 second half
    for (int i = 0; i < kWT; ++i)
        g_excWT[2 * kWT + i] = (i < kWT / 2) ? 1.0f : -1.0f;
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t*) {
    req.numParameters = ARRAY_SIZE(parameters);
    req.sram = sizeof(DualPhaserAlg);
    req.dram = 0;
    req.dtc  = sizeof(DualPhaserAlg_DTC);
    req.itc  = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs,
                          const _NT_algorithmRequirements&,
                          const int32_t*) {
    DualPhaserAlg_DTC* d = (DualPhaserAlg_DTC*)ptrs.dtc;
    memset(d, 0, sizeof(*d));

    DualPhaserAlg* a = new (ptrs.sram) DualPhaserAlg(d);
    a->parameters     = parameters;
    a->parameterPages = &paramPages;

    // AP coefficients from default Phase values
    const float theta0 = 500.0f * kPi / 2000.0f;
    a->ap1_c1 = 2.0f * kAP_R * cosf(theta0);
    a->ap1_c2 = kAP_R * kAP_R;
    a->ap2_c1 = a->ap1_c1;
    a->ap2_c2 = a->ap1_c2;

    recalcHPF(a);
    recalcLPF(a);
    recalcExciter(a, kParamExciterFreq);
    recalcExciter(a, kParamAttack);
    recalcExciter(a, kParamHold);
    recalcExciter(a, kParamRelease);

    // HoldTrigPhase only makes sense in Exciter mode; gray it out at startup (default=Mix)
    {
        const int32_t idx = NT_algorithmIndex(a);
        NT_setParameterGrayedOut(idx, kParamHoldTrigPhase + NT_parameterOffset(), true);
    }

    return a;
}

void parameterChanged(_NT_algorithm* self, int p) {
    DualPhaserAlg* a = (DualPhaserAlg*)self;
    const bool excMode      = (a->v[kParamInputToOutput] == 1);
    const bool holdTrig     = excMode && (a->v[kParamHoldTrigPhase] == 1);
    switch (p) {
    case kParamHPF:       recalcHPF(a);        break;
    case kParamLPF:       recalcLPF(a);        break;
    case kParamPhase1:
    case kParamFeedback1:
        if (!holdTrig) recalcPhaser(a, 0);
        break;
    case kParamPhase2:
    case kParamFeedback2:
        if (!holdTrig) recalcPhaser(a, 1);
        break;
    case kParamHoldTrigPhase:
        // Turning OFF: flush any deferred phase changes immediately
        if (a->v[kParamHoldTrigPhase] == 0) {
            recalcPhaser(a, 0);
            recalcPhaser(a, 1);
        }
        break;
    case kParamInputToOutput: {
        const int32_t idx = NT_algorithmIndex(self);
        NT_setParameterGrayedOut(idx, kParamHoldTrigPhase + NT_parameterOffset(), !excMode);
        // Leaving Exciter mode: apply phases immediately in case they were deferred
        if (!excMode) {
            recalcPhaser(a, 0);
            recalcPhaser(a, 1);
        }
        break;
    }
    case kParamExciterFreq:
    case kParamAttack:
    case kParamHold:
    case kParamRelease:   recalcExciter(a, p); break;
    }
}

// ─── Audio processing ─────────────────────────────────────────────────────────
//
//  Mix mode   (InputToOutput = 0):
//    chainIn = xin (audio)
//    p1out   = 0.5·xin  + 0.5·AP1_out
//    p2out   = 0.5·p1out + 0.5·AP2_out
//
//  Exciter mode (InputToOutput = 1):
//    xin     = trigger source (rising edge > 1V fires envelope + resets phase)
//    chainIn = exciter oscillator × AHD envelope  (±10V peak)
//    p1out   = AP1_out  (no dry mix)
//    p2out   = AP2_out  (no dry mix)
//
//  Per phaser chain:  feedback summer → HPF → LPF → 24× AP → output summer
//
void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    DualPhaserAlg*     a = (DualPhaserAlg*)self;
    DualPhaserAlg_DTC* d = a->dtc;

    const int    n   = numFramesBy4 * 4;
    const float* in  = busFrames + (a->v[kParamInput]  - 1) * n;
    float*       out = busFrames + (a->v[kParamOutput] - 1) * n;
    const bool   rep = (a->v[kParamOutputMode] != 0);

    // Hoist all buffer-constant values
    const float c1a = a->ap1_c1, c2a = a->ap1_c2, f1 = a->fb1;
    const float c1b = a->ap2_c1, c2b = a->ap2_c2, f2 = a->fb2;

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

        // ── Exciter (active only in Exciter mode) ─────────────────────────
        float excSig = 0.0f;
        if (excMode) {
            // Rising-edge trigger: resets oscillator phase and fires envelope
            if (xin >= 1.0f && d->excPrevIn < 1.0f) {
                d->excPhase  = 0.0f;
                d->excEnvVal = 0.0f;
                d->excState  = 1;          // restart attack
                d->excRemain = excAtkS;
                // HoldTrigPhase: latch deferred phase/feedback parameter changes
                if (holdTrigPhase) {
                    recalcPhaser(a, 0);
                    recalcPhaser(a, 1);
                }
            }
            d->excPrevIn = xin;

            // AHD envelope state machine
            float env = d->excEnvVal;
            switch (d->excState) {
            case 1: // Attack — exponential rise toward 10V
                env += (10.0f - env) * excAtkC;
                if (--d->excRemain <= 0) {
                    env = 10.0f;           // force exact peak to avoid any ramp artifact
                    d->excState  = 2;
                    d->excRemain = excHldS;
                }
                break;
            case 2: // Hold — constant 10V
                env = 10.0f;
                if (--d->excRemain <= 0)
                    d->excState = 3;
                break;
            case 3: // Release — exponential decay toward 0V
                env *= excRelC;
                if (env < 0.001f) { env = 0.0f; d->excState = 0; }
                break;
            default: // Idle
                env = 0.0f;
                break;
            }
            d->excEnvVal = env;

            // Oscillator — only advances and reads when envelope is non-zero
            if (env > 0.0f) {
                const float* tbl = g_excWT + excWave * kWT;
                const float  pos = d->excPhase * (float)kWT;
                const int    i0  = (int)pos & (kWT - 1);
                const float  t   = pos - (float)i0;
                // env ∈ [0,10] → wave scaled to ±10V at peak
                excSig = (tbl[i0] + t * (tbl[(i0 + 1) & (kWT - 1)] - tbl[i0])) * env;

                d->excPhase += excPInc;
                if (d->excPhase >= 1.0f) d->excPhase -= 1.0f;
            }
        }

        // chainIn: exciter signal (Exciter mode) or audio input (Mix mode)
        const float chainIn1 = excMode ? excSig : xin;

        // ── Phaser 1 ──────────────────────────────────────────────────────
        float sig = chainIn1 * (1.0f - f1) + d->prev_out1 * f1;

        // HPF — 4th-order Butterworth
        {
            float hx = sig;
            float hy = hb0*(hx - 2.0f*d->h1_x1[0] + d->h1_x2[0])
                       - ha1_0*d->h1_y1[0] - ha2_0*d->h1_y2[0];
            d->h1_x2[0]=d->h1_x1[0]; d->h1_x1[0]=hx;
            d->h1_y2[0]=d->h1_y1[0]; d->h1_y1[0]=hy; hx=hy;
            hy = hb1*(hx - 2.0f*d->h1_x1[1] + d->h1_x2[1])
                 - ha1_1*d->h1_y1[1] - ha2_1*d->h1_y2[1];
            d->h1_x2[1]=d->h1_x1[1]; d->h1_x1[1]=hx;
            d->h1_y2[1]=d->h1_y1[1]; d->h1_y1[1]=hy; sig=hy;
        }
        // LPF — 4th-order Butterworth
        {
            float lx = sig;
            float ly = lb0*(lx + 2.0f*d->l1_x1[0] + d->l1_x2[0])
                       - la1_0*d->l1_y1[0] - la2_0*d->l1_y2[0];
            d->l1_x2[0]=d->l1_x1[0]; d->l1_x1[0]=lx;
            d->l1_y2[0]=d->l1_y1[0]; d->l1_y1[0]=ly; lx=ly;
            ly = lb1*(lx + 2.0f*d->l1_x1[1] + d->l1_x2[1])
                 - la1_1*d->l1_y1[1] - la2_1*d->l1_y2[1];
            d->l1_x2[1]=d->l1_x1[1]; d->l1_x1[1]=lx;
            d->l1_y2[1]=d->l1_y1[1]; d->l1_y1[1]=ly; sig=ly;
        }
        // 24× second-order all-pass stages
        for (int s = 0; s < 24; ++s) {
            const float x1=d->ap1_x1[s], x2=d->ap1_x2[s];
            const float y1=d->ap1_y1[s], y2=d->ap1_y2[s];
            const float y = c2a*sig - c1a*x1 + x2 + c1a*y1 - c2a*y2;
            d->ap1_x2[s]=x1; d->ap1_x1[s]=sig;
            d->ap1_y2[s]=y1; d->ap1_y1[s]=y;
            sig=y;
        }
        // Output summer — Exciter mode: wet only; Mix mode: 50/50 dry+wet
        const float p1out = excMode ? sig : (0.5f * xin + 0.5f * sig);
        d->prev_out1 = p1out > 11.0f ? 11.0f : p1out < -11.0f ? -11.0f : p1out;

        // ── Phaser 2 (input = phaser 1 output) ───────────────────────────
        sig = p1out * (1.0f - f2) + d->prev_out2 * f2;

        // HPF
        {
            float hx = sig;
            float hy = hb0*(hx - 2.0f*d->h2_x1[0] + d->h2_x2[0])
                       - ha1_0*d->h2_y1[0] - ha2_0*d->h2_y2[0];
            d->h2_x2[0]=d->h2_x1[0]; d->h2_x1[0]=hx;
            d->h2_y2[0]=d->h2_y1[0]; d->h2_y1[0]=hy; hx=hy;
            hy = hb1*(hx - 2.0f*d->h2_x1[1] + d->h2_x2[1])
                 - ha1_1*d->h2_y1[1] - ha2_1*d->h2_y2[1];
            d->h2_x2[1]=d->h2_x1[1]; d->h2_x1[1]=hx;
            d->h2_y2[1]=d->h2_y1[1]; d->h2_y1[1]=hy; sig=hy;
        }
        // LPF
        {
            float lx = sig;
            float ly = lb0*(lx + 2.0f*d->l2_x1[0] + d->l2_x2[0])
                       - la1_0*d->l2_y1[0] - la2_0*d->l2_y2[0];
            d->l2_x2[0]=d->l2_x1[0]; d->l2_x1[0]=lx;
            d->l2_y2[0]=d->l2_y1[0]; d->l2_y1[0]=ly; lx=ly;
            ly = lb1*(lx + 2.0f*d->l2_x1[1] + d->l2_x2[1])
                 - la1_1*d->l2_y1[1] - la2_1*d->l2_y2[1];
            d->l2_x2[1]=d->l2_x1[1]; d->l2_x1[1]=lx;
            d->l2_y2[1]=d->l2_y1[1]; d->l2_y1[1]=ly; sig=ly;
        }
        // 24× AP
        for (int s = 0; s < 24; ++s) {
            const float x1=d->ap2_x1[s], x2=d->ap2_x2[s];
            const float y1=d->ap2_y1[s], y2=d->ap2_y2[s];
            const float y = c2b*sig - c1b*x1 + x2 + c1b*y1 - c2b*y2;
            d->ap2_x2[s]=x1; d->ap2_x1[s]=sig;
            d->ap2_y2[s]=y1; d->ap2_y1[s]=y;
            sig=y;
        }
        const float p2out = excMode ? sig : (0.5f * p1out + 0.5f * sig);
        d->prev_out2 = p2out > 11.0f ? 11.0f : p2out < -11.0f ? -11.0f : p2out;

        const float clamped = p2out > 11.0f ? 11.0f : p2out < -11.0f ? -11.0f : p2out;
        out[i] = rep ? clamped : out[i] + clamped;
    }
}

// ─── Factory ──────────────────────────────────────────────────────────────────
static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('D','u','P','h'),
    .name                        = "DualPhaser",
    .description                 = "Dual 24-stage all-pass phaser with triggered exciter",
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
