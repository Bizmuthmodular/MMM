#include "distingnt/api.h"
#include <cmath>
#include <cstdint>
#include <new>

// ---------------------------------------------------------------------------
// Chord data
// ---------------------------------------------------------------------------

static const char* const chordNames[] = {
    "maj",    "min",    "dim",    "aug",    "sus2",   "sus4",   "5",     "maj(b5)",
    "maj6",   "min6",   "dom7",   "maj7",   "min7",   "mMaj7",  "dim7",  "m7b5",
    "aug7",   "augM7",  "7sus4",  "add9",   "madd9",  "6/9",    "m6/9",  "maj7#11",
    "7b9",
    NULL
};

static const char* const rootNames[] = {
    "C", "D", "E", "F", "G", "A", "B", NULL
};

// Intervals in semitones, 4 voices per chord type.
// Triads (indices 0-7): voice 4 (index 3) = -12 (root one octave below OSC1).
static const int8_t chordIntervals[25][4] = {
    {  0,  4,  7, -12 }, //  0 maj
    {  0,  3,  7, -12 }, //  1 min
    {  0,  3,  6, -12 }, //  2 dim
    {  0,  4,  8, -12 }, //  3 aug
    {  0,  2,  7, -12 }, //  4 sus2
    {  0,  5,  7, -12 }, //  5 sus4
    {  0,  7, 12, -12 }, //  6 5 (power)
    {  0,  4,  6, -12 }, //  7 maj(b5)
    {  0,  4,  7,   9 }, //  8 maj6
    {  0,  3,  7,   9 }, //  9 min6
    {  0,  4,  7,  10 }, // 10 dom7
    {  0,  4,  7,  11 }, // 11 maj7
    {  0,  3,  7,  10 }, // 12 min7
    {  0,  3,  7,  11 }, // 13 mMaj7
    {  0,  3,  6,   9 }, // 14 dim7
    {  0,  3,  6,  10 }, // 15 m7b5
    {  0,  4,  8,  10 }, // 16 aug7
    {  0,  4,  8,  11 }, // 17 augM7
    {  0,  5,  7,  10 }, // 18 7sus4
    {  0,  4,  7,  14 }, // 19 add9
    {  0,  3,  7,  14 }, // 20 madd9
    {  0,  4,  9,  14 }, // 21 6/9
    {  0,  3,  9,  14 }, // 22 m6/9
    {  0,  4, 11,  18 }, // 23 maj7#11
    {  0,  4, 10,  13 }, // 24 7b9
};

// Semitone offsets for roots C D E F G A B
static const int8_t rootSemitones[7] = { 0, 2, 4, 5, 7, 9, 11 };

// C3 reference frequency: 130.8128 Hz
static const float kC3Hz = 130.8128f;

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

enum {
    kParamChord1 = 0, kParamRoot1,
    kParamChord2,     kParamRoot2,
    kParamChord3,     kParamRoot3,
    kParamChord4,     kParamRoot4,
    kParamChord5,     kParamRoot5,
    kParamChord6,     kParamRoot6,
    kParamChord7,     kParamRoot7,
    kParamChord8,     kParamRoot8,
    kParamProgLength, // 16
    kParamAdvanceIn,  // 17
    kParamOSC1Level,  // 18
    kParamOSC2Level,  // 19
    kParamOSC3Level,  // 20
    kParamOSC4Level,  // 21
    kParamOutput,     // 22
    kNumParams        // 23
};

#define CHORD_PARAM(n) \
    { .name="Chord " #n, .min=0, .max=24, .def=0, \
      .unit=kNT_unitEnum, .scaling=0, .enumStrings=chordNames },
#define ROOT_PARAM(n) \
    { .name="Root " #n,  .min=0, .max=6,  .def=0, \
      .unit=kNT_unitEnum, .scaling=0, .enumStrings=rootNames },

static const _NT_parameter parameters[kNumParams] = {
    CHORD_PARAM(1) ROOT_PARAM(1)
    CHORD_PARAM(2) ROOT_PARAM(2)
    CHORD_PARAM(3) ROOT_PARAM(3)
    CHORD_PARAM(4) ROOT_PARAM(4)
    CHORD_PARAM(5) ROOT_PARAM(5)
    CHORD_PARAM(6) ROOT_PARAM(6)
    CHORD_PARAM(7) ROOT_PARAM(7)
    CHORD_PARAM(8) ROOT_PARAM(8)
    { .name="ProgLength", .min=1, .max=8, .def=4,
      .unit=kNT_unitNone, .scaling=0, .enumStrings=NULL },
    NT_PARAMETER_CV_INPUT( "AdvanceIn", 0, 0 )
    { .name="OSC1 Level", .min=0, .max=100, .def=100,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name="OSC2 Level", .min=0, .max=100, .def=100,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name="OSC3 Level", .min=0, .max=100, .def=100,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name="OSC4 Level", .min=0, .max=100, .def=100,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )
};

// ---------------------------------------------------------------------------
// Parameter pages
// ---------------------------------------------------------------------------

static const uint8_t progressionPageParams[] = {
    kParamChord1, kParamRoot1,
    kParamChord2, kParamRoot2,
    kParamChord3, kParamRoot3,
    kParamChord4, kParamRoot4,
    kParamChord5, kParamRoot5,
    kParamChord6, kParamRoot6,
    kParamChord7, kParamRoot7,
    kParamChord8, kParamRoot8,
    kParamProgLength,
    kParamAdvanceIn,
};

static const uint8_t oscillatorsPageParams[] = {
    kParamOSC1Level, kParamOSC2Level, kParamOSC3Level, kParamOSC4Level,
    kParamOutput,
};

static const _NT_parameterPage pages[] = {
    { .name="Progression", .numParams=ARRAY_SIZE(progressionPageParams), .group=0, .params=progressionPageParams },
    { .name="Oscillators",  .numParams=ARRAY_SIZE(oscillatorsPageParams), .group=0, .params=oscillatorsPageParams },
};

static const _NT_parameterPages paramPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages    = pages,
};

// ---------------------------------------------------------------------------
// Algorithm structs
// ---------------------------------------------------------------------------

struct ChordsAlg_DTC {
    float osc_phase[4];
    float osc_freq[4];
    float osc_levelV[4]; // peak amplitude in volts (0..10)
    float prevAdvTrig;
};

struct ChordsAlg : public _NT_algorithm {
    ChordsAlg_DTC* dtc;
    int currentStep;

    ChordsAlg(ChordsAlg_DTC* d) : dtc(d), currentStep(0) {}
};

// ---------------------------------------------------------------------------
// DSP helpers
// ---------------------------------------------------------------------------

static inline float polyblep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t*t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t*t + t + t + 1.0f;
    }
    return 0.0f;
}

// Called only on chord/root/step change — powf calls are acceptable here.
static void updateFrequencies(ChordsAlg* a) {
    ChordsAlg_DTC* d = a->dtc;
    int step      = a->currentStep;
    int chordType = a->v[kParamChord1 + step * 2];
    int rootIdx   = a->v[kParamRoot1  + step * 2];
    float rootHz  = kC3Hz * powf(2.0f, rootSemitones[rootIdx] / 12.0f);
    for (int i = 0; i < 4; ++i)
        d->osc_freq[i] = rootHz * powf(2.0f, (float)chordIntervals[chordType][i] / 12.0f);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t*) {
    req.numParameters = kNumParams;
    req.sram = sizeof(ChordsAlg);
    req.dtc  = sizeof(ChordsAlg_DTC);
    req.dram = 0;
    req.itc  = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs,
                         const _NT_algorithmRequirements&,
                         const int32_t*)
{
    ChordsAlg_DTC* d = (ChordsAlg_DTC*)ptrs.dtc;

    // Initialise to C major at C3 (default chord=maj[0], root=C[0]).
    // Intervals: [0, 4, 7, -12] — do not call updateFrequencies() here
    // because v[] is not yet populated during construct().
    static const float kMajIntervals[4] = { 0.0f, 4.0f, 7.0f, -12.0f };
    for (int i = 0; i < 4; ++i) {
        d->osc_phase[i]  = 0.0f;
        d->osc_freq[i]   = kC3Hz * powf(2.0f, kMajIntervals[i] / 12.0f);
        d->osc_levelV[i] = 10.0f; // matches def=100 → 100/100*10 = 10V
    }
    d->prevAdvTrig = 0.0f;

    ChordsAlg* a = new (ptrs.sram) ChordsAlg(d);
    a->parameters     = parameters;
    a->parameterPages = &paramPages;
    return a;
}

void parameterChanged(_NT_algorithm* self, int p) {
    ChordsAlg* a = (ChordsAlg*)self;
    if (p >= kParamChord1 && p <= kParamRoot8) {
        updateFrequencies(a);
    } else if (p == kParamProgLength) {
        if (a->currentStep >= a->v[kParamProgLength])
            a->currentStep = 0;
        updateFrequencies(a);
    } else if (p >= kParamOSC1Level && p <= kParamOSC4Level) {
        a->dtc->osc_levelV[p - kParamOSC1Level] = a->v[p] / 100.0f * 10.0f;
    }
}

// ---------------------------------------------------------------------------
// step()
// ---------------------------------------------------------------------------

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    ChordsAlg*     a = (ChordsAlg*)self;
    ChordsAlg_DTC* d = a->dtc;
    const int numFrames = numFramesBy4 * 4;
    const float sr = NT_globals.sampleRate;

    float* out = busFrames + (a->v[kParamOutput] - 1) * numFrames;

    const int advBus = a->v[kParamAdvanceIn];
    const float* adv = (advBus > 0) ? busFrames + (advBus - 1) * numFrames : nullptr;

    const int progLength = a->v[kParamProgLength];

    // Load DTC into locals for the hot loop
    float prevTrig = d->prevAdvTrig;
    float ph0 = d->osc_phase[0], ph1 = d->osc_phase[1];
    float ph2 = d->osc_phase[2], ph3 = d->osc_phase[3];
    float f0  = d->osc_freq[0],  f1  = d->osc_freq[1];
    float f2  = d->osc_freq[2],  f3  = d->osc_freq[3];
    const float lv0 = d->osc_levelV[0], lv1 = d->osc_levelV[1];
    const float lv2 = d->osc_levelV[2], lv3 = d->osc_levelV[3];
    float dt0 = f0/sr, dt1 = f1/sr, dt2 = f2/sr, dt3 = f3/sr;

    for (int i = 0; i < numFrames; ++i) {
        // Rising-edge detection on AdvanceIn — advance progression
        if (adv) {
            const float t = adv[i];
            if (prevTrig < 1.0f && t >= 1.0f) {
                a->currentStep = (a->currentStep + 1) % progLength;
                updateFrequencies(a);
                f0 = d->osc_freq[0]; f1 = d->osc_freq[1];
                f2 = d->osc_freq[2]; f3 = d->osc_freq[3];
                dt0 = f0/sr; dt1 = f1/sr; dt2 = f2/sr; dt3 = f3/sr;
            }
            prevTrig = t;
        }

        // PolyBLEP anti-aliased sawtooth for each oscillator
        const float saw0 = 2.0f*ph0 - 1.0f - polyblep(ph0, dt0);
        const float saw1 = 2.0f*ph1 - 1.0f - polyblep(ph1, dt1);
        const float saw2 = 2.0f*ph2 - 1.0f - polyblep(ph2, dt2);
        const float saw3 = 2.0f*ph3 - 1.0f - polyblep(ph3, dt3);

        out[i] = saw0*lv0 + saw1*lv1 + saw2*lv2 + saw3*lv3;

        ph0 += dt0; if (ph0 >= 1.0f) ph0 -= 1.0f;
        ph1 += dt1; if (ph1 >= 1.0f) ph1 -= 1.0f;
        ph2 += dt2; if (ph2 >= 1.0f) ph2 -= 1.0f;
        ph3 += dt3; if (ph3 >= 1.0f) ph3 -= 1.0f;
    }

    d->prevAdvTrig  = prevTrig;
    d->osc_phase[0] = ph0; d->osc_phase[1] = ph1;
    d->osc_phase[2] = ph2; d->osc_phase[3] = ph3;
    d->osc_freq[0]  = f0;  d->osc_freq[1]  = f1;
    d->osc_freq[2]  = f2;  d->osc_freq[3]  = f3;
}

// ---------------------------------------------------------------------------
// Factory and entry point
// ---------------------------------------------------------------------------

static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('C','H','R','D'),
    .name                        = "Chords",
    .description                 = "4-voice chord oscillator with progression sequencer",
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
