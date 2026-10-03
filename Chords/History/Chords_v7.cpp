#include "distingnt/api.h"
#include <cmath>
#include <cstdint>
#include <cstring>
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

// Full chromatic root notes C0–C5 (61 entries)
static const char* const rootNames[] = {
    "C0",  "C#0", "D0",  "D#0", "E0",  "F0",  "F#0", "G0",  "G#0", "A0",  "A#0", "B0",
    "C1",  "C#1", "D1",  "D#1", "E1",  "F1",  "F#1", "G1",  "G#1", "A1",  "A#1", "B1",
    "C2",  "C#2", "D2",  "D#2", "E2",  "F2",  "F#2", "G2",  "G#2", "A2",  "A#2", "B2",
    "C3",  "C#3", "D3",  "D#3", "E3",  "F3",  "F#3", "G3",  "G#3", "A3",  "A#3", "B3",
    "C4",  "C#4", "D4",  "D#4", "E4",  "F4",  "F#4", "G4",  "G#4", "A4",  "A#4", "B4",
    "C5",
    NULL
};

// Note letter names (no octave), used when building the display string
static const char* const noteLetters[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
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

// C0 reference: C3 (130.8128 Hz) is 3 octaves above C0
static const float kC0Hz = 130.8128f / 8.0f; // 16.3516 Hz

// ---------------------------------------------------------------------------
// Parameters
// Root is first within each pair; index 36 = C3 is default root.
// ---------------------------------------------------------------------------

enum {
    kParamRoot1 = 0, kParamChord1,   //  0,  1
    kParamRoot2,     kParamChord2,   //  2,  3
    kParamRoot3,     kParamChord3,   //  4,  5
    kParamRoot4,     kParamChord4,   //  6,  7
    kParamRoot5,     kParamChord5,   //  8,  9
    kParamRoot6,     kParamChord6,   // 10, 11
    kParamRoot7,     kParamChord7,   // 12, 13
    kParamRoot8,     kParamChord8,   // 14, 15
    kParamProgLength, // 16
    kParamAdvanceIn,  // 17
    kParamResetIn,    // 18
    kParamProgDiv,    // 19
    kParamOSC1Level,  // 20
    kParamOSC2Level,  // 21
    kParamOSC3Level,  // 22
    kParamOSC4Level,  // 23
    kParamOutput,     // 24
    kNumParams        // 25
};

#define ROOT_PARAM(n) \
    { .name="Root " #n,  .min=0, .max=60, .def=36, \
      .unit=kNT_unitEnum, .scaling=0, .enumStrings=rootNames },
#define CHORD_PARAM(n) \
    { .name="Chord " #n, .min=0, .max=24, .def=0, \
      .unit=kNT_unitEnum, .scaling=0, .enumStrings=chordNames },

static const _NT_parameter parameters[kNumParams] = {
    ROOT_PARAM(1) CHORD_PARAM(1)
    ROOT_PARAM(2) CHORD_PARAM(2)
    ROOT_PARAM(3) CHORD_PARAM(3)
    ROOT_PARAM(4) CHORD_PARAM(4)
    ROOT_PARAM(5) CHORD_PARAM(5)
    ROOT_PARAM(6) CHORD_PARAM(6)
    ROOT_PARAM(7) CHORD_PARAM(7)
    ROOT_PARAM(8) CHORD_PARAM(8)
    { .name="ProgLength", .min=1, .max=8, .def=4,
      .unit=kNT_unitNone, .scaling=0, .enumStrings=NULL },
    NT_PARAMETER_CV_INPUT( "AdvanceIn", 0, 0 )
    NT_PARAMETER_CV_INPUT( "Reset", 0, 0 )
    { .name="ProgDiv", .min=1, .max=128, .def=1,
      .unit=kNT_unitNone, .scaling=0, .enumStrings=NULL },
    { .name="OSC1 Level", .min=0, .max=100, .def=25,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name="OSC2 Level", .min=0, .max=100, .def=25,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name="OSC3 Level", .min=0, .max=100, .def=25,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    { .name="OSC4 Level", .min=0, .max=100, .def=25,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )
};

// ---------------------------------------------------------------------------
// Parameter pages
// ---------------------------------------------------------------------------

static const uint8_t progressionPageParams[] = {
    kParamRoot1, kParamChord1,
    kParamRoot2, kParamChord2,
    kParamRoot3, kParamChord3,
    kParamRoot4, kParamChord4,
    kParamRoot5, kParamChord5,
    kParamRoot6, kParamChord6,
    kParamRoot7, kParamChord7,
    kParamRoot8, kParamChord8,
    kParamProgLength,
    kParamAdvanceIn,
    kParamResetIn,
    kParamProgDiv,
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
    float prevResetTrig;
};

struct ChordsAlg : public _NT_algorithm {
    ChordsAlg_DTC* dtc;
    int currentStep;
    int advDivCount; // counts incoming advance triggers; advances step every v[kParamProgDiv] triggers

    ChordsAlg(ChordsAlg_DTC* d) : dtc(d), currentStep(0), advDivCount(0) {}
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
    int rootIdx   = a->v[kParamRoot1  + step * 2]; // 0..60, C0..C5
    int chordType = a->v[kParamChord1 + step * 2];
    float rootHz  = kC0Hz * powf(2.0f, (float)rootIdx / 12.0f);
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

    // Initialise to C3 major (default root=36=C3, chord=0=maj).
    // Intervals: [0, 4, 7, -12]. Cannot call updateFrequencies() here
    // because v[] is not yet populated during construct().
    static const float kMajIntervals[4] = { 0.0f, 4.0f, 7.0f, -12.0f };
    // rootIdx=36 → kC0Hz * 2^(36/12) = kC0Hz * 8 = kC3Hz
    const float kC3Hz = kC0Hz * 8.0f;
    for (int i = 0; i < 4; ++i) {
        d->osc_phase[i]  = 0.0f;
        d->osc_freq[i]   = kC3Hz * powf(2.0f, kMajIntervals[i] / 12.0f);
        d->osc_levelV[i] = 2.5f; // matches def=25 → 25/100*10 = 2.5V
    }
    d->prevAdvTrig   = 0.0f;
    d->prevResetTrig = 0.0f;

    ChordsAlg* a = new (ptrs.sram) ChordsAlg(d);
    a->parameters     = parameters;
    a->parameterPages = &paramPages;
    return a;
}

void parameterChanged(_NT_algorithm* self, int p) {
    ChordsAlg* a = (ChordsAlg*)self;
    if (p >= kParamRoot1 && p <= kParamChord8) {
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
// draw() — progression overview
//
// Layout (256×64 display):
//   8 slots × 32px wide = 256px (full width, always allocated for 8 chords)
//   Slots beyond ProgLength are left blank.
//   Each active slot (up to 3 rows, all kNT_textNormal, max 5 chars each):
//     chordY1 (y=30): note + chord prefix,  e.g. "Fmaj"  or "Fmaj7" if it fits
//     chordY2 (y=39): chord suffix if split, e.g. "7#11"  (skipped when no split)
//     octaveY (y=48): octave digit,          e.g. "3"
//   Split rule (applied to combined = note+chord):
//     Backward scan from pos 5 → find rightmost word-boundary ≤5.
//     Word boundaries: alpha↔non-alpha, or lowercase→uppercase (camelCase).
//     Fallback forward scan if none found within 5.
//     No split if combined ≤ 5 chars.
//   Active (playing) slot: unfilled box frame y=20..49, full brightness (col=15)
//   Inactive slots within ProgLength: half brightness (col=8), no frame
//   Returns false so the firmware header bar (current param/value) is shown.
// ---------------------------------------------------------------------------

bool draw(_NT_algorithm* self) {
    ChordsAlg* a          = (ChordsAlg*)self;
    const int progLength  = a->v[kParamProgLength];
    const int activeStep  = a->currentStep;

    const int slotW   = 32;  // 8 * 32 = 256 = full display width
    const int chordY1 = 30;  // note letter + chord prefix
    const int chordY2 = 39;  // chord suffix (only when split)
    const int octaveY = 48;  // octave digit
    const int frameY0 = 20;
    const int frameY1 = 49;

    for (int s = 0; s < 8; ++s) {
        if (s >= progLength) continue;

        const int rootIdx   = a->v[kParamRoot1  + s * 2];
        const int chordType = a->v[kParamChord1 + s * 2];
        const int noteIdx   = rootIdx % 12;
        const int octave    = rootIdx / 12;
        const int cx        = s * slotW + slotW / 2; // centre x of slot
        const int col       = (s == activeStep) ? 15 : 8;

        // Active chord: draw unfilled bounding box
        if (s == activeStep) {
            NT_drawShapeI(kNT_box,
                          s * slotW + 1, frameY0,
                          s * slotW + slotW - 2, frameY1,
                          15);
        }

        // Build combined string: note letter(s) + chord name, max 15 chars
        char combined[16];
        strcpy(combined, noteLetters[noteIdx]);
        strcat(combined, chordNames[chordType]);
        const int totalLen = (int)strlen(combined);

        // Find split position in combined (index where line 2 begins).
        // Rule: rightmost word-boundary at or before position 5.
        // Boundaries: alpha↔non-alpha, lowercase→uppercase.
        // Fallback: first boundary found anywhere if none within 5.
        int splitPos = -1;
        if (totalLen > 5) {
            // Backward scan from position 5 — rightmost boundary ≤ 5
            for (int k = 5; k >= 1 && splitPos < 0; --k) {
                const char p = combined[k-1], c = combined[k];
                const bool pA = (p>='a'&&p<='z')||(p>='A'&&p<='Z');
                const bool cA = (c>='a'&&c<='z')||(c>='A'&&c<='Z');
                const bool pL = (p>='a'&&p<='z');
                const bool cU = (c>='A'&&c<='Z');
                if ((pA&&!cA)||(!pA&&cA)||(pL&&cU))
                    splitPos = k;
            }
            // Fallback: forward scan for first boundary anywhere
            for (int k = 1; k < totalLen && splitPos < 0; ++k) {
                const char p = combined[k-1], c = combined[k];
                const bool pA = (p>='a'&&p<='z')||(p>='A'&&p<='Z');
                const bool cA = (c>='a'&&c<='z')||(c>='A'&&c<='Z');
                const bool pL = (p>='a'&&p<='z');
                const bool cU = (c>='A'&&c<='Z');
                if ((pA&&!cA)||(!pA&&cA)||(pL&&cU))
                    splitPos = k;
            }
        }

        if (splitPos < 0) {
            // No split needed: combined fits on one line
            NT_drawText(cx, chordY1, combined, col, kNT_textCentre, kNT_textNormal);
        } else {
            // Line 1: combined[0..splitPos-1]
            char line1[8];
            memcpy(line1, combined, (size_t)splitPos);
            line1[splitPos] = '\0';
            NT_drawText(cx, chordY1, line1,           col, kNT_textCentre, kNT_textNormal);
            // Line 2: combined[splitPos..]
            NT_drawText(cx, chordY2, combined+splitPos, col, kNT_textCentre, kNT_textNormal);
        }

        // Octave digit
        const char octStr[2] = { (char)('0' + octave), 0 };
        NT_drawText(cx, octaveY, octStr, col, kNT_textCentre, kNT_textNormal);
    }

    return false; // allow firmware to draw the standard header bar
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

    const int rstBus = a->v[kParamResetIn];
    const float* rst = (rstBus > 0) ? busFrames + (rstBus - 1) * numFrames : nullptr;

    const int progLength = a->v[kParamProgLength];
    const int progDiv    = a->v[kParamProgDiv];

    // Load DTC into locals for the hot loop
    float prevTrig  = d->prevAdvTrig;
    float prevReset = d->prevResetTrig;
    float ph0 = d->osc_phase[0], ph1 = d->osc_phase[1];
    float ph2 = d->osc_phase[2], ph3 = d->osc_phase[3];
    float f0  = d->osc_freq[0],  f1  = d->osc_freq[1];
    float f2  = d->osc_freq[2],  f3  = d->osc_freq[3];
    const float lv0 = d->osc_levelV[0], lv1 = d->osc_levelV[1];
    const float lv2 = d->osc_levelV[2], lv3 = d->osc_levelV[3];
    float dt0 = f0/sr, dt1 = f1/sr, dt2 = f2/sr, dt3 = f3/sr;

    for (int i = 0; i < numFrames; ++i) {
        // Rising-edge detection on ResetIn — return to step 0 (1V threshold)
        if (rst) {
            const float r = rst[i];
            if (prevReset < 1.0f && r >= 1.0f) {
                a->currentStep = 0;
                updateFrequencies(a);
                f0 = d->osc_freq[0]; f1 = d->osc_freq[1];
                f2 = d->osc_freq[2]; f3 = d->osc_freq[3];
                dt0 = f0/sr; dt1 = f1/sr; dt2 = f2/sr; dt3 = f3/sr;
            }
            prevReset = r;
        }

        // Rising-edge detection on AdvanceIn — advance progression
        if (adv) {
            const float t = adv[i];
            if (prevTrig < 1.0f && t >= 1.0f) {
                if (++a->advDivCount >= progDiv) {
                    a->advDivCount = 0;
                    a->currentStep = (a->currentStep + 1) % progLength;
                    updateFrequencies(a);
                    f0 = d->osc_freq[0]; f1 = d->osc_freq[1];
                    f2 = d->osc_freq[2]; f3 = d->osc_freq[3];
                    dt0 = f0/sr; dt1 = f1/sr; dt2 = f2/sr; dt3 = f3/sr;
                }
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

    d->prevAdvTrig   = prevTrig;
    d->prevResetTrig = prevReset;
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
    .draw                        = draw,
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
