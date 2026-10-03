#include "distingnt/api.h"
#include <cmath>
#include <cstring>
#include <new>

// ─── Constants ───────────────────────────────────────────────────────────────
static const float kPi    = 3.14159265358979f;
static const float kAP_R  = 0.9f;   // pole radius — sets Q of each all-pass stage

// ─── DTC  (fastest TCM memory — all per-sample state lives here) ─────────────
struct DualPhaserAlg_DTC {
    // Phaser 1: 24 second-order all-pass stages
    float ap1_x1[24], ap1_x2[24];   // input  delays  x[n-1], x[n-2]
    float ap1_y1[24], ap1_y2[24];   // output delays  y[n-1], y[n-2]
    float prev_out1;                  // phaser 1 output(t-1) used as feedback
    // Phaser 1: 24 dB/oct HPF pre-filter (2 second-order stages)
    float h1_x1[2], h1_x2[2], h1_y1[2], h1_y2[2];
    // Phaser 2: same layout
    float ap2_x1[24], ap2_x2[24];
    float ap2_y1[24], ap2_y2[24];
    float prev_out2;
    // Phaser 2: HPF pre-filter
    float h2_x1[2], h2_x2[2], h2_y1[2], h2_y2[2];
};

// ─── Algorithm struct ────────────────────────────────────────────────────────
struct DualPhaserAlg : public _NT_algorithm {
    DualPhaserAlg_DTC* dtc;
    // Cached AP biquad coefficients
    //   y[n] = c2*x[n] - c1*x[n-1] + x[n-2] + c1*y[n-1] - c2*y[n-2]
    //   c1 = 2*R*cos(θ),  c2 = R²,  θ = pole angle (0…π/2)
    float ap1_c1, ap1_c2;
    float ap2_c1, ap2_c2;
    // Feedback ratios [0, 2]  (>1.0 amplifies feedback path)
    float fb1, fb2;
    // 4th-order Butterworth HPF at 30 Hz — two biquad stages (shared both phasers)
    //   y[n] = b0*(x[n] - 2*x[n-1] + x[n-2]) - a1*y[n-1] - a2*y[n-2]
    float hpf_b0[2], hpf_a1[2], hpf_a2[2];

    DualPhaserAlg(DualPhaserAlg_DTC* d)
        : dtc(d),
          ap1_c1(0.0f), ap1_c2(kAP_R * kAP_R),
          ap2_c1(0.0f), ap2_c2(kAP_R * kAP_R),
          fb1(0.0f), fb2(0.0f) {}
};

// ─── Parameters ──────────────────────────────────────────────────────────────
enum {
    kParamInput,
    kParamOutput,
    kParamOutputMode,
    kParamPhase1,        // 0-1000, pole sweep DC→π/2 (quarter Nyquist)
    kParamFeedback1,     // 0-2000 → 0.0-200.0%
    kParamPhase2,
    kParamFeedback2,
    kNumParams
};

static const _NT_parameter parameters[] = {
    NT_PARAMETER_AUDIO_INPUT("Input", 1, 1)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Output", 1, 13)
    { .name="Phase 1",    .min=0, .max=1000, .def=500,
      .unit=kNT_unitNone,    .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Feedback 1", .min=0, .max=2000, .def=0,
      .unit=kNT_unitPercent, .scaling=kNT_scaling10,   .enumStrings=NULL },
    { .name="Phase 2",    .min=0, .max=1000, .def=500,
      .unit=kNT_unitNone,    .scaling=kNT_scalingNone, .enumStrings=NULL },
    { .name="Feedback 2", .min=0, .max=2000, .def=0,
      .unit=kNT_unitPercent, .scaling=kNT_scaling10,   .enumStrings=NULL },
};

// ─── Pages ───────────────────────────────────────────────────────────────────
static const uint8_t phasersPageParams[] = {
    kParamPhase1, kParamFeedback1, kParamPhase2, kParamFeedback2
};
static const uint8_t routingPageParams[] = {
    kParamInput, kParamOutput, kParamOutputMode
};
static const _NT_parameterPage pages[] = {
    { .name="Phasers", .numParams=ARRAY_SIZE(phasersPageParams), .group=0, .params=phasersPageParams },
    { .name="Routing", .numParams=ARRAY_SIZE(routingPageParams), .group=0, .params=routingPageParams },
};
static const _NT_parameterPages paramPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages    = pages,
};

// ─── Coefficient update ───────────────────────────────────────────────────────
//  Phase 0-1000 maps pole angle θ linearly from 0 to π/2
//  (0 = poles at DC, 1000 = poles at quarter Nyquist ≈ 12 kHz @ 48 kHz)
//  Feedback 0-2000 → ratio 0.0-2.0 (>1.0 amplifies the fed-back signal)
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

    // Initialise AP coefficients from default values (parameterChanged not called at construct)
    // Default Phase = 500 → θ = 500 * π / 2000 = π/4
    const float theta0 = 500.0f * kPi / 2000.0f;
    a->ap1_c1 = 2.0f * kAP_R * cosf(theta0);
    a->ap1_c2 = kAP_R * kAP_R;
    a->ap2_c1 = a->ap1_c1;
    a->ap2_c2 = a->ap1_c2;
    // fb1 = fb2 = 0 already set in constructor

    // ── 4th-order Butterworth HPF at 30 Hz ──────────────────────────────────
    // Two biquad stages with Q values for a Butterworth 4th-order factorisation:
    //   Q[0] = 1/(2*cos(67.5°)) ≈ 1.3066  (high-Q inner pair)
    //   Q[1] = 1/(2*cos(22.5°)) ≈ 0.5412  (low-Q outer pair)
    // HPF biquad via bilinear transform (pre-warped):
    //   K = tan(π * fc / sr)
    //   norm = 1 + K/Q + K²
    //   b0 = 1/norm,  b1 = -2*b0,  b2 = b0
    //   a1 = 2*(K²-1)/norm,  a2 = (1 - K/Q + K²)/norm
    {
        const float fc  = 30.0f;
        const float sr  = NT_globals.sampleRate;
        const float K   = tanf(kPi * fc / sr);
        const float K2  = K * K;
        const float Qs[2] = { 1.3066f, 0.5412f };
        for (int s = 0; s < 2; ++s) {
            const float norm  = 1.0f + K / Qs[s] + K2;
            a->hpf_b0[s] = 1.0f / norm;
            a->hpf_a1[s] = 2.0f * (K2 - 1.0f) / norm;
            a->hpf_a2[s] = (1.0f - K / Qs[s] + K2) / norm;
        }
    }

    return a;
}

void parameterChanged(_NT_algorithm* self, int p) {
    DualPhaserAlg* a = (DualPhaserAlg*)self;
    switch (p) {
    case kParamPhase1:
    case kParamFeedback1: recalcPhaser(a, 0); break;
    case kParamPhase2:
    case kParamFeedback2: recalcPhaser(a, 1); break;
    }
}

// ─── Audio processing ─────────────────────────────────────────────────────────
//
//  Per phaser, topology:
//
//    xin ──┬──────────────────────────────────────────────┐
//          │                                              ▼
//        [×(1-fb)] ◄── prev_out (t-1) ──[×fb] → [+] → HPF(30Hz,24dB/oct)
//                                                          │
//                                               24× 2nd-order AP stages
//                                                          │
//    xin ──────────────────────────────────── [+½] ◄───────┘
//                                              │
//                                          phaser_out (→ next phaser / output)
//
//  All-pass biquad (per stage):
//    y[n] = c2·x[n] - c1·x[n-1] + x[n-2] + c1·y[n-1] - c2·y[n-2]
//    c1 = 2·R·cos(θ),  c2 = R²,  θ ∈ [0, π/2]
//
//  HPF biquad (per stage, 2 cascaded = 24 dB/oct):
//    y[n] = b0·(x[n] - 2·x[n-1] + x[n-2]) - a1·y[n-1] - a2·y[n-2]
//
void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    DualPhaserAlg*     a = (DualPhaserAlg*)self;
    DualPhaserAlg_DTC* d = a->dtc;

    const int    n   = numFramesBy4 * 4;
    const float* in  = busFrames + (a->v[kParamInput]  - 1) * n;
    float*       out = busFrames + (a->v[kParamOutput] - 1) * n;
    const bool   rep = (a->v[kParamOutputMode] != 0);

    // Hoist coefficients into registers
    const float c1a = a->ap1_c1, c2a = a->ap1_c2, f1 = a->fb1;
    const float c1b = a->ap2_c1, c2b = a->ap2_c2, f2 = a->fb2;
    const float hb0 = a->hpf_b0[0], ha1_0 = a->hpf_a1[0], ha2_0 = a->hpf_a2[0];
    const float hb1 = a->hpf_b0[1], ha1_1 = a->hpf_a1[1], ha2_1 = a->hpf_a2[1];

    for (int i = 0; i < n; ++i) {
        const float xin = in[i];

        // ── Phaser 1 ──────────────────────────────────────────────────────
        float sig = xin * (1.0f - f1) + d->prev_out1 * f1;

        // 24 dB/oct HPF at 30 Hz (2 cascaded biquad stages)
        {
            float hx = sig;
            // Stage 0
            float hy = hb0*(hx - 2.0f*d->h1_x1[0] + d->h1_x2[0])
                       - ha1_0*d->h1_y1[0] - ha2_0*d->h1_y2[0];
            d->h1_x2[0] = d->h1_x1[0];  d->h1_x1[0] = hx;
            d->h1_y2[0] = d->h1_y1[0];  d->h1_y1[0] = hy;
            hx = hy;
            // Stage 1
            hy = hb1*(hx - 2.0f*d->h1_x1[1] + d->h1_x2[1])
                 - ha1_1*d->h1_y1[1] - ha2_1*d->h1_y2[1];
            d->h1_x2[1] = d->h1_x1[1];  d->h1_x1[1] = hx;
            d->h1_y2[1] = d->h1_y1[1];  d->h1_y1[1] = hy;
            sig = hy;
        }

        for (int s = 0; s < 24; ++s) {
            const float x1 = d->ap1_x1[s], x2 = d->ap1_x2[s];
            const float y1 = d->ap1_y1[s], y2 = d->ap1_y2[s];
            const float y  = c2a*sig - c1a*x1 + x2 + c1a*y1 - c2a*y2;
            d->ap1_x2[s] = x1;  d->ap1_x1[s] = sig;
            d->ap1_y2[s] = y1;  d->ap1_y1[s] = y;
            sig = y;
        }

        const float p1out = 0.5f * xin + 0.5f * sig;
        d->prev_out1 = p1out > 11.0f ? 11.0f : p1out < -11.0f ? -11.0f : p1out;

        // ── Phaser 2 (input = phaser 1 output) ───────────────────────────
        sig = p1out * (1.0f - f2) + d->prev_out2 * f2;

        // 24 dB/oct HPF at 30 Hz
        {
            float hx = sig;
            float hy = hb0*(hx - 2.0f*d->h2_x1[0] + d->h2_x2[0])
                       - ha1_0*d->h2_y1[0] - ha2_0*d->h2_y2[0];
            d->h2_x2[0] = d->h2_x1[0];  d->h2_x1[0] = hx;
            d->h2_y2[0] = d->h2_y1[0];  d->h2_y1[0] = hy;
            hx = hy;
            hy = hb1*(hx - 2.0f*d->h2_x1[1] + d->h2_x2[1])
                 - ha1_1*d->h2_y1[1] - ha2_1*d->h2_y2[1];
            d->h2_x2[1] = d->h2_x1[1];  d->h2_x1[1] = hx;
            d->h2_y2[1] = d->h2_y1[1];  d->h2_y1[1] = hy;
            sig = hy;
        }

        for (int s = 0; s < 24; ++s) {
            const float x1 = d->ap2_x1[s], x2 = d->ap2_x2[s];
            const float y1 = d->ap2_y1[s], y2 = d->ap2_y2[s];
            const float y  = c2b*sig - c1b*x1 + x2 + c1b*y1 - c2b*y2;
            d->ap2_x2[s] = x1;  d->ap2_x1[s] = sig;
            d->ap2_y2[s] = y1;  d->ap2_y1[s] = y;
            sig = y;
        }

        const float p2out = 0.5f * p1out + 0.5f * sig;
        d->prev_out2 = p2out > 11.0f ? 11.0f : p2out < -11.0f ? -11.0f : p2out;

        const float clamped = p2out > 11.0f ? 11.0f : p2out < -11.0f ? -11.0f : p2out;
        out[i] = rep ? clamped : out[i] + clamped;
    }
}

// ─── Factory ──────────────────────────────────────────────────────────────────
static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('D','u','P','h'),
    .name                        = "DualPhaser",
    .description                 = "Dual 24-stage 2nd-order all-pass phaser",
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
