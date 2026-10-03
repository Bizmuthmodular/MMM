#include "distingnt/api.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

// ---------------------------------------------------------------------------
// Chord data
// ---------------------------------------------------------------------------

static const char* const chordNames[] = {
    "uni", "maj", "min", "7", "maj7", "min7", "sus2", "sus4", "tri",
    "6/9", "m7b5", "dim7", "9", "maj9", "min9", "min6", "7sus4",
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
static const int8_t chordIntervals[17][4] = {
    {  0,  0,  0,   0 }, //  0 uni
    {  0,  4,  7, -12 }, //  1 maj
    {  0,  3,  7, -12 }, //  2 min
    {  0,  4,  7,  10 }, //  3 7
    {  0,  4,  7,  11 }, //  4 maj7
    {  0,  3,  7,  10 }, //  5 min7
    {  0,  2,  7, -12 }, //  6 sus2
    {  0,  5,  7, -12 }, //  7 sus4
    {  0,  6, 12,  18 }, //  8 tri
    {  0,  4,  9,  14 }, //  9 6/9
    {  0,  3,  6,  10 }, // 10 m7b5
    {  0,  3,  6,   9 }, // 11 dim7
    {  0,  4, 10,  14 }, // 12 9
    {  0,  4, 11,  14 }, // 13 maj9
    {  0,  3, 10,  14 }, // 14 min9
    {  0,  3,  7,   9 }, // 15 min6
    {  0,  5,  7,  10 }, // 16 7sus4
};

// C0 reference
static const float kC0Hz = 130.8128f / 8.0f; // 16.3516 Hz
static const float kPi   = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Parameters
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
    kParamOSC1Shape,  // 21
    kParamOSC2Level,  // 22
    kParamOSC2Shape,  // 23
    kParamOSC3Level,  // 24
    kParamOSC3Shape,  // 25
    kParamOSC4Level,  // 26
    kParamOSC4Shape,  // 27
    kParamDetune,     // 28
    kParamOutput,     // 29
    kParamLevelFreq,  // 30
    kParamLevelMod,   // 31
    kParamShapeFreq,  // 32
    kParamShapeMod,   // 33
    kParamFMFreq,     // 34
    kParamFMDepth,    // 35
    kParamPhase,      // 36
    kParamFMPrePost,  // 37
    kParamOctave,     // 38
    kParamBassWaveform, // 39
    kParamBassLevel,    // 40
    kParamBassOutput,   // 41
    kParamClockIn,      // 42
    kParamBassRange,    // 43
    kParamBassOffset,   // 44
    kParamRandomise,    // 45
    kParamLoopLength,   // 46
    kParamGlide,        // 47
    kParamBassDensity,  // 48
    kParamGlideCurve,   // 49
    kParamNoteUpdate,   // 50
    // Melody page
    kParamMelOSC1Wave,  // 51
    kParamMelLevel,     // 52
    kParamMelOutput,    // 53
    kParamMelClockIn,   // 54
    kParamMelRange,     // 55
    kParamMelOffset,    // 56
    kParamMelRandomise, // 57
    kParamMelLoopLength,// 58
    kParamMelGlide,     // 59
    kParamMelDensity,   // 60
    kParamMelGlideCurve,// 61
    kParamMelNoteUpdate,// 62
    kParamMelOctave,    // 63
    // Melody synth
    kParamMelOSC2Wave,    // 64
    kParamMelAlgo,        // 65
    kParamMelIndex,       // 66
    kParamMelOSC2Partial, // 67
    kParamMelOSC2Semi,    // 68
    kNumParams            // 69
};

#define ROOT_PARAM(n) \
    { .name="Root " #n,  .min=0, .max=60, .def=36, \
      .unit=kNT_unitEnum, .scaling=0, .enumStrings=rootNames },
#define CHORD_PARAM(n) \
    { .name="Chord " #n, .min=0, .max=16, .def=1, \
      .unit=kNT_unitEnum, .scaling=0, .enumStrings=chordNames },

static const char* const fmPrePostNames[]    = { "pre", "post", NULL };
static const char* const bassWaveformNames[] = { "sine", "tri",  NULL };
static const char* const melWaveNames[]      = { "Sine", "Tri", "Saw", "Square", NULL };
static const char* const melAlgoNames[]      = { "Mix", "FM", "Swarm", "PM", "AM", NULL };

#define SHAPE_PARAM(n) \
    { .name="OSC" #n " Shape", .min=0, .max=1000, .def=0, \
      .unit=kNT_unitNone, .scaling=0, .enumStrings=NULL },

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
    SHAPE_PARAM(1)
    { .name="OSC2 Level", .min=0, .max=100, .def=25,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    SHAPE_PARAM(2)
    { .name="OSC3 Level", .min=0, .max=100, .def=25,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    SHAPE_PARAM(3)
    { .name="OSC4 Level", .min=0, .max=100, .def=25,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    SHAPE_PARAM(4)
    { .name="Detune", .min=0, .max=33, .def=0,
      .unit=kNT_unitCents, .scaling=0, .enumStrings=NULL },
    NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )
    { .name="LevelFreq", .min=1,   .max=4400, .def=10,
      .unit=kNT_unitHz,      .scaling=kNT_scaling10, .enumStrings=NULL },
    { .name="LevelMod",  .min=0,   .max=100,  .def=0,
      .unit=kNT_unitPercent, .scaling=0,             .enumStrings=NULL },
    { .name="ShapeFreq", .min=1,   .max=4400, .def=10,
      .unit=kNT_unitHz,      .scaling=kNT_scaling10, .enumStrings=NULL },
    { .name="ShapeMod",  .min=0,   .max=100,  .def=0,
      .unit=kNT_unitPercent, .scaling=0,             .enumStrings=NULL },
    { .name="FMFreq",    .min=-4,  .max=16,   .def=1,
      .unit=kNT_unitNone,    .scaling=0,             .enumStrings=NULL },
    { .name="FMDepth",   .min=0,   .max=1000, .def=0,
      .unit=kNT_unitPercent, .scaling=kNT_scaling10, .enumStrings=NULL },
    { .name="Phase",     .min=0,   .max=3600, .def=0,
      .unit=kNT_unitDegrees, .scaling=kNT_scaling10, .enumStrings=NULL },
    { .name="FMpre/post",.min=0,   .max=1,    .def=0,
      .unit=kNT_unitEnum,    .scaling=0,             .enumStrings=fmPrePostNames },
    { .name="Octave",    .min=-2,  .max=2,    .def=0,
      .unit=kNT_unitNone,    .scaling=0,             .enumStrings=NULL },
    // Bass page
    { .name="BassWave",    .min=0,  .max=1,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=bassWaveformNames },
    { .name="BassLevel",   .min=0,  .max=100, .def=50,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
    NT_PARAMETER_AUDIO_OUTPUT( "BassOut", 1, 14 )
    NT_PARAMETER_CV_INPUT( "ClockIn", 0, 0 )
    { .name="BassRange",   .min=0,  .max=15,  .def=7,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="BassOffset",  .min=0,  .max=3,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="Randomise",   .min=0,  .max=1,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="LoopLength",  .min=1,  .max=16,  .def=8,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="Glide",       .min=0,  .max=100, .def=0,
      .unit=kNT_unitSeconds, .scaling=kNT_scaling100,  .enumStrings=NULL },
    { .name="BassDensity", .min=0,  .max=100, .def=75,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
    { .name="GlideCurve",  .min=0,  .max=100, .def=50,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="NoteUpdate",  .min=0,  .max=8,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // Melody sequencer page (index 51+)
    { .name="OSC1 Wave",   .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=melWaveNames },
    { .name="MelLevel",    .min=0,  .max=100, .def=50,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
    NT_PARAMETER_AUDIO_OUTPUT( "MelOut", 1, 15 )
    NT_PARAMETER_CV_INPUT( "ClockIn.", 0, 0 )
    { .name="MelRange",    .min=0,  .max=15,  .def=7,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="MelOffset",   .min=-4, .max=3,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="Randomise.",  .min=0,  .max=1,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="LoopLength.", .min=1,  .max=16,  .def=8,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="Glide.",      .min=0,  .max=100, .def=0,
      .unit=kNT_unitSeconds, .scaling=kNT_scaling100,  .enumStrings=NULL },
    { .name="MelDensity",  .min=0,  .max=100, .def=75,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
    { .name="GlideCurve.", .min=0,  .max=100, .def=50,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="NoteUpdate.", .min=0,  .max=8,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="MelOctave",   .min=-4, .max=4,    .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // Melody synth page
    { .name="OSC2 Wave",   .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=melWaveNames },
    { .name="Algo",        .min=0,  .max=4,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=melAlgoNames },
    { .name="Index",       .min=0,  .max=1000,.def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="OSC2 Part",   .min=0,  .max=12,  .def=1,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="OSC2 Semi",   .min=-24,.max=24,  .def=0,
      .unit=kNT_unitSemitones,.scaling=0,              .enumStrings=NULL },
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
    kParamOSC1Level, kParamOSC1Shape,
    kParamOSC2Level, kParamOSC2Shape,
    kParamOSC3Level, kParamOSC3Shape,
    kParamOSC4Level, kParamOSC4Shape,
    kParamOctave,
    kParamDetune,
    kParamFMFreq,
    kParamFMDepth,
    kParamFMPrePost,
    kParamPhase,
    kParamOutput,
};

static const uint8_t quadLFOPageParams[] = {
    kParamLevelFreq, kParamLevelMod,
    kParamShapeFreq, kParamShapeMod,
};

static const uint8_t bassPageParams[] = {
    kParamBassWaveform,
    kParamBassLevel,
    kParamBassOutput,
    kParamClockIn,
    kParamBassRange,
    kParamBassOffset,
    kParamRandomise,
    kParamLoopLength,
    kParamGlide,
    kParamGlideCurve,
    kParamBassDensity,
    kParamNoteUpdate,
};

static const uint8_t melodyPageParams[] = {
    kParamMelOSC1Wave,
    kParamMelOSC2Wave,
    kParamMelAlgo,
    kParamMelIndex,
    kParamMelOSC2Partial,
    kParamMelOSC2Semi,
    kParamMelLevel,
    kParamMelOutput,
    kParamMelClockIn,
    kParamMelRange,
    kParamMelOffset,
    kParamMelOctave,
    kParamMelRandomise,
    kParamMelLoopLength,
    kParamMelGlide,
    kParamMelGlideCurve,
    kParamMelDensity,
    kParamMelNoteUpdate,
};

static const _NT_parameterPage pages[] = {
    { .name="Progression", .numParams=ARRAY_SIZE(progressionPageParams), .group=0, .params=progressionPageParams },
    { .name="Oscillators",  .numParams=ARRAY_SIZE(oscillatorsPageParams), .group=0, .params=oscillatorsPageParams },
    { .name="QuadLFO",      .numParams=ARRAY_SIZE(quadLFOPageParams),     .group=0, .params=quadLFOPageParams },
    { .name="Bass",         .numParams=ARRAY_SIZE(bassPageParams),         .group=0, .params=bassPageParams },
    { .name="Melody",       .numParams=ARRAY_SIZE(melodyPageParams),       .group=0, .params=melodyPageParams },
};

static const _NT_parameterPages paramPages = {
    .numPages = 5,
    .pages    = pages,
};

// ---------------------------------------------------------------------------
// Algorithm structs
// ---------------------------------------------------------------------------

struct ChordsAlg_DTC {
    float osc_phase[4];
    float fmPhase[4];
    float osc_freq[4];
    float osc_levelV[4];
    float prevAdvTrig;
    float prevResetTrig;
    float lfoX;
    float lfoY;
    float lfo2X;
    float lfo2Y;
    // Bass oscillator
    float bassPhase;
    float bassFreq;
    float bassTargetFreq;
    float bassGlideStartFreq;
    float bassGlideProgress;
    float bassGlideDT;
    float bassLevelV;
    float prevClockTrig;
    float prevBassSample;
    // Melody oscillator 1
    float melPhase;
    float melFreq;
    float melTargetFreq;
    float melGlideStartFreq;
    float melGlideProgress;
    float melGlideDT;
    float melLevelV;
    float prevMelClockTrig;
    float prevMelSample;
    // Melody oscillator 2
    float mel2Phase;
    // Swarm copy phases (7 copies of OSC1 at detuned pitches)
    float melSwarmPhase[7];
};

struct ChordsAlg : public _NT_algorithm {
    ChordsAlg_DTC* dtc;
    int currentStep;
    int   advDivCount;
    float lfoCos;
    float lfoSin;
    float levelModDepth;
    float lfoCos2;
    float lfoSin2;
    float shapeModDepth;
    float osc_gainF[4];
    float fmFreqMult;
    float fmDepth;
    float phaseStep;
    // Bass
    float   notePool[16];
    float   melodyLoop[16];
    float   densityLoop[16];
    int     bassLoopPos;
    bool    bassGated;
    bool    bassPendingGated;
    bool    bassHasPending;
    int16_t bassRandomisePrev;
    int     bassNoteUpdateCount;
    bool    bassHasPendingNote;
    float   bassNoteUpdateTargetHz;
    // Melody sequencer
    float   melNotePool[16];
    float   melMelodyLoop[16];
    float   melDensityLoop[16];
    int     melLoopPos;
    bool    melGated;
    bool    melPendingGated;
    bool    melHasPending;
    int16_t melRandomisePrev;
    int     melNoteUpdateCount;
    bool    melHasPendingNote;
    float   melNoteUpdateTargetHz;
    float   melOctaveShift;
    // Melody synth cached params
    float   melOSC2FreqScale;  // partial * 2^(semi/12); updated on OSC2Partial/Semi change
    float   melSwarmRatio[7];  // 2^((i+1)*centsPerCopy/1200) for i=0..6; updated on Index change

    ChordsAlg(ChordsAlg_DTC* d) : dtc(d), currentStep(0), advDivCount(0),
                                   lfoCos(1.0f), lfoSin(0.0f), levelModDepth(0.0f),
                                   lfoCos2(1.0f), lfoSin2(0.0f), shapeModDepth(0.0f),
                                   fmFreqMult(1.0f), fmDepth(0.0f), phaseStep(0.0f),
                                   bassLoopPos(0), bassGated(false),
                                   bassPendingGated(false), bassHasPending(false),
                                   bassRandomisePrev(0),
                                   bassNoteUpdateCount(0), bassHasPendingNote(false),
                                   bassNoteUpdateTargetHz(0.0f),
                                   melLoopPos(0), melGated(false),
                                   melPendingGated(false), melHasPending(false),
                                   melRandomisePrev(0),
                                   melNoteUpdateCount(0), melHasPendingNote(false),
                                   melNoteUpdateTargetHz(0.0f),
                                   melOctaveShift(1.0f),
                                   melOSC2FreqScale(1.0f) {
        osc_gainF[0] = osc_gainF[1] = osc_gainF[2] = osc_gainF[3] = 1.0f;
        for (int i = 0; i < 16; ++i) notePool[i] = 0.0f;
        for (int i = 0; i < 16; ++i) { melodyLoop[i] = 0.5f; densityLoop[i] = 0.5f; }
        for (int i = 0; i < 16; ++i) melNotePool[i] = 0.0f;
        for (int i = 0; i < 16; ++i) { melMelodyLoop[i] = 0.5f; melDensityLoop[i] = 0.5f; }
        for (int i = 0; i < 7; ++i) melSwarmRatio[i] = 1.0f;
    }
};

// ---------------------------------------------------------------------------
// DSP helpers
// ---------------------------------------------------------------------------

static uint32_t lcg_state = 0x12345678u;
static inline uint32_t lcg_next() {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

// Wavefolder for chord oscillators (wfold still used by chord engine)
static inline float wfold(float x) {
    x += 1.0f;
    float t = x - 4.0f * floorf(x * 0.25f);
    return (t < 2.0f) ? (t - 1.0f) : (3.0f - t);
}

// PolyBLEP correction — reduces aliasing at waveform discontinuities.
// t = phase [0,1), dt = phase increment per sample.
static inline float polyblep(float t, float dt) {
    if (dt <= 0.0f) return 0.0f;
    if (t < dt) {
        t /= dt;
        return t + t - t*t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t*t + t + t + 1.0f;
    }
    return 0.0f;
}

// Band-limited waveform generator with PolyBLEP for Saw and Square.
// wave: 0=Sine, 1=Triangle, 2=Saw, 3=Square
static inline float oscWave(int wave, float phase, float dt) {
    switch (wave) {
    default:
    case 0: // Sine
        return sinf(2.0f * kPi * phase);
    case 1: // Triangle — no discontinuities, no correction needed
        return (phase < 0.25f) ?  phase * 4.0f
             : (phase < 0.75f) ?  2.0f - phase * 4.0f
                               : (phase - 1.0f) * 4.0f;
    case 2: { // Saw — discontinuity at phase wraps; correct with PolyBLEP
        float s = 2.0f * phase - 1.0f;
        s -= polyblep(phase, dt);
        return s;
    }
    case 3: { // Square — discontinuities at 0 and 0.5; correct both
        float s = phase < 0.5f ? 1.0f : -1.0f;
        s += polyblep(phase, dt);
        float p2 = phase + 0.5f;
        if (p2 >= 1.0f) p2 -= 1.0f;
        s -= polyblep(p2, dt);
        return s;
    }
    }
}

// Build the 16-note bass pool from current chord, sorted ascending.
static void updateNotePool(ChordsAlg* a) {
    int step      = a->currentStep;
    int rootIdx   = a->v[kParamRoot1  + step * 2];
    int chordType = a->v[kParamChord1 + step * 2];
    float rootHz  = kC0Hz * powf(2.0f, (float)rootIdx / 12.0f);
    int np = 0;
    for (int i = 0; i < 4; ++i) {
        const float baseHz = rootHz * powf(2.0f, (float)chordIntervals[chordType][i] / 12.0f);
        for (int oct = -2; oct <= 1; ++oct)
            a->notePool[np++] = baseHz * powf(2.0f, (float)oct);
    }
    for (int i = 1; i < 16; ++i) {
        const float key = a->notePool[i];
        int j = i - 1;
        while (j >= 0 && a->notePool[j] > key) { a->notePool[j+1] = a->notePool[j]; --j; }
        a->notePool[j+1] = key;
    }
}

// Build the 16-note melody pool, MelOctave shifts the octave range.
static void updateMelNotePool(ChordsAlg* a) {
    int step      = a->currentStep;
    int rootIdx   = a->v[kParamRoot1  + step * 2];
    int chordType = a->v[kParamChord1 + step * 2];
    float rootHz  = kC0Hz * powf(2.0f, (float)rootIdx / 12.0f);
    const int octOff = a->v[kParamMelOctave];
    a->melOctaveShift = powf(2.0f, (float)octOff);
    int np = 0;
    for (int i = 0; i < 4; ++i) {
        const float baseHz = rootHz * powf(2.0f, (float)chordIntervals[chordType][i] / 12.0f);
        for (int oct = 2 + octOff; oct <= 5 + octOff; ++oct)
            a->melNotePool[np++] = baseHz * powf(2.0f, (float)oct);
    }
    for (int i = 1; i < 16; ++i) {
        const float key = a->melNotePool[i];
        int j = i - 1;
        while (j >= 0 && a->melNotePool[j] > key) { a->melNotePool[j+1] = a->melNotePool[j]; --j; }
        a->melNotePool[j+1] = key;
    }
}

static void updateFrequencies(ChordsAlg* a) {
    ChordsAlg_DTC* d = a->dtc;
    int step      = a->currentStep;
    int rootIdx   = a->v[kParamRoot1  + step * 2];
    int chordType = a->v[kParamChord1 + step * 2];
    float rootHz  = kC0Hz * powf(2.0f, (float)rootIdx / 12.0f + (float)a->v[kParamOctave]);
    const int detuneCents = a->v[kParamDetune];
    for (int i = 0; i < 4; ++i) {
        const float baseHz = rootHz * powf(2.0f, (float)chordIntervals[chordType][i] / 12.0f);
        d->osc_freq[i] = (i == 0) ? baseHz
                                   : baseHz * powf(2.0f, (float)(i * detuneCents) / 1200.0f);
    }
    updateNotePool(a);

    {
        ChordsAlg_DTC* bd = a->dtc;
        if (bd->bassFreq > 0.0f && a->notePool[0] > 0.0f) {
            float closestHz   = a->notePool[0];
            float closestDist = fabsf(logf(bd->bassFreq / a->notePool[0]));
            for (int i = 1; i < 16; ++i) {
                const float dist = fabsf(logf(bd->bassFreq / a->notePool[i]));
                if (dist < closestDist) { closestDist = dist; closestHz = a->notePool[i]; }
            }
            bd->bassTargetFreq = closestHz;
            const float glideSecs = a->v[kParamGlide] * 0.01f;
            if (glideSecs <= 0.0f || bd->bassFreq == closestHz) {
                bd->bassFreq         = closestHz;
                bd->bassGlideProgress = 1.0f;
                bd->bassGlideDT      = 0.0f;
            } else {
                bd->bassGlideStartFreq = bd->bassFreq;
                bd->bassGlideProgress  = 0.0f;
                const float samps = glideSecs * NT_globals.sampleRate;
                bd->bassGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
            }
        }
    }

    updateMelNotePool(a);
    {
        ChordsAlg_DTC* md = a->dtc;
        if (md->melFreq > 0.0f && a->melNotePool[0] > 0.0f) {
            float closestHz   = a->melNotePool[0];
            float closestDist = fabsf(logf(md->melFreq / a->melNotePool[0]));
            for (int i = 1; i < 16; ++i) {
                const float dist = fabsf(logf(md->melFreq / a->melNotePool[i]));
                if (dist < closestDist) { closestDist = dist; closestHz = a->melNotePool[i]; }
            }
            md->melTargetFreq = closestHz;
            const float glideSecs = a->v[kParamMelGlide] * 0.01f;
            if (glideSecs <= 0.0f || md->melFreq == closestHz) {
                md->melFreq         = closestHz;
                md->melGlideProgress = 1.0f;
                md->melGlideDT      = 0.0f;
            } else {
                md->melGlideStartFreq = md->melFreq;
                md->melGlideProgress  = 0.0f;
                const float samps = glideSecs * NT_globals.sampleRate;
                md->melGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
            }
        }
    }
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

    static const float kMajIntervals[4] = { 0.0f, 4.0f, 7.0f, -12.0f };
    const float kC3Hz = kC0Hz * 8.0f;
    for (int i = 0; i < 4; ++i) {
        d->osc_phase[i]  = 0.0f;
        d->fmPhase[i]    = 0.0f;
        d->osc_freq[i]   = kC3Hz * powf(2.0f, kMajIntervals[i] / 12.0f);
        d->osc_levelV[i] = 2.5f;
    }
    d->prevAdvTrig   = 0.0f;
    d->prevResetTrig = 0.0f;
    d->lfoX          = 1.0f;
    d->lfoY          = 0.0f;
    d->lfo2X         = 1.0f;
    d->lfo2Y         = 0.0f;
    d->bassPhase          = 0.0f;
    d->bassFreq           = kC0Hz * 4.0f;
    d->bassTargetFreq     = d->bassFreq;
    d->bassGlideStartFreq = d->bassFreq;
    d->bassGlideProgress  = 1.0f;
    d->bassGlideDT        = 0.0f;
    d->bassLevelV         = 5.0f;
    d->prevClockTrig      = 0.0f;
    d->prevBassSample     = 0.0f;
    d->melPhase           = 0.0f;
    d->melFreq            = kC0Hz * 32.0f;
    d->melTargetFreq      = d->melFreq;
    d->melGlideStartFreq  = d->melFreq;
    d->melGlideProgress   = 1.0f;
    d->melGlideDT         = 0.0f;
    d->melLevelV          = 5.0f;
    d->prevMelClockTrig   = 0.0f;
    d->prevMelSample      = 0.0f;
    d->mel2Phase          = 0.0f;
    for (int i = 0; i < 7; ++i) d->melSwarmPhase[i] = 0.0f;

    ChordsAlg* a = new (ptrs.sram) ChordsAlg(d);
    a->parameters     = parameters;
    a->parameterPages = &paramPages;
    a->lfoCos        = cosf(2.0f * kPi * 1.0f / NT_globals.sampleRate);
    a->lfoSin        = sinf(2.0f * kPi * 1.0f / NT_globals.sampleRate);
    a->levelModDepth = 0.0f;
    a->lfoCos2       = cosf(2.0f * kPi * 1.0f / NT_globals.sampleRate);
    a->lfoSin2       = sinf(2.0f * kPi * 1.0f / NT_globals.sampleRate);
    a->shapeModDepth = 0.0f;
    for (int i = 0; i < 4; ++i)
        a->osc_gainF[i] = 1.0f + a->v[kParamOSC1Shape + i*2] * 8.0f / 1000.0f;
    // Init default note pool: C3 major
    {
        const float kC3Hz_ = kC0Hz * 8.0f;
        static const int8_t majI[4] = { 0, 4, 7, -12 };
        int np = 0;
        for (int i = 0; i < 4; ++i) {
            const float bHz = kC3Hz_ * powf(2.0f, (float)majI[i] / 12.0f);
            for (int oct = -2; oct <= 1; ++oct)
                a->notePool[np++] = bHz * powf(2.0f, (float)oct);
        }
        for (int i = 1; i < 16; ++i) {
            const float key = a->notePool[i];
            int j = i - 1;
            while (j >= 0 && a->notePool[j] > key) { a->notePool[j+1] = a->notePool[j]; --j; }
            a->notePool[j+1] = key;
        }
        d->bassFreq           = a->notePool[0];
        d->bassTargetFreq     = a->notePool[0];
        d->bassGlideStartFreq = a->notePool[0];
    }
    // Init melody note pool: C3 major, octaves +2..+5
    {
        const float kC3Hz_ = kC0Hz * 8.0f;
        static const int8_t majI[4] = { 0, 4, 7, -12 };
        int np = 0;
        for (int i = 0; i < 4; ++i) {
            const float bHz = kC3Hz_ * powf(2.0f, (float)majI[i] / 12.0f);
            for (int oct = 2; oct <= 5; ++oct)
                a->melNotePool[np++] = bHz * powf(2.0f, (float)oct);
        }
        for (int i = 1; i < 16; ++i) {
            const float key = a->melNotePool[i];
            int j = i - 1;
            while (j >= 0 && a->melNotePool[j] > key) { a->melNotePool[j+1] = a->melNotePool[j]; --j; }
            a->melNotePool[j+1] = key;
        }
        d->melFreq           = a->melNotePool[8];
        d->melTargetFreq     = d->melFreq;
        d->melGlideStartFreq = d->melFreq;
    }
    // Init melody synth: default partial=1, semi=0 → scale=1.0; index=0 → swarm ratios=1.0
    // (already set in constructor)
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
    } else if (p >= kParamOSC1Level && p <= kParamOSC4Shape) {
        const int oscIdx = (p - kParamOSC1Level) / 2;
        if ((p - kParamOSC1Level) % 2 == 0)
            a->dtc->osc_levelV[oscIdx] = a->v[p] / 100.0f * 10.0f;
        else
            a->osc_gainF[oscIdx] = 1.0f + a->v[p] * 8.0f / 1000.0f;
    } else if (p == kParamDetune) {
        updateFrequencies(a);
    } else if (p == kParamLevelFreq) {
        const float lfoHz = a->v[kParamLevelFreq] * 0.1f;
        a->lfoCos = cosf(2.0f * kPi * lfoHz / NT_globals.sampleRate);
        a->lfoSin = sinf(2.0f * kPi * lfoHz / NT_globals.sampleRate);
    } else if (p == kParamLevelMod) {
        a->levelModDepth = a->v[kParamLevelMod] / 100.0f;
    } else if (p == kParamShapeFreq) {
        const float lfo2Hz = a->v[kParamShapeFreq] * 0.1f;
        a->lfoCos2 = cosf(2.0f * kPi * lfo2Hz / NT_globals.sampleRate);
        a->lfoSin2 = sinf(2.0f * kPi * lfo2Hz / NT_globals.sampleRate);
    } else if (p == kParamShapeMod) {
        a->shapeModDepth = a->v[kParamShapeMod] / 100.0f;
    } else if (p == kParamFMFreq) {
        a->fmFreqMult = (float)a->v[kParamFMFreq];
    } else if (p == kParamFMDepth) {
        a->fmDepth = a->v[kParamFMDepth] * 0.01f;
    } else if (p == kParamPhase) {
        a->phaseStep = a->v[kParamPhase] / 3600.0f;
    } else if (p == kParamOctave) {
        updateFrequencies(a);
    } else if (p == kParamBassLevel) {
        a->dtc->bassLevelV = a->v[kParamBassLevel] / 100.0f * 10.0f;
    } else if (p == kParamRandomise) {
        if (a->bassRandomisePrev == 0 && a->v[kParamRandomise] == 1) {
            for (int i = 0; i < 16; ++i) {
                a->melodyLoop[i]  = (float)(lcg_next() & 0xFFFFu) / 65535.0f;
                a->densityLoop[i] = (float)(lcg_next() & 0xFFFFu) / 65535.0f;
            }
        }
        a->bassRandomisePrev = a->v[kParamRandomise];
    } else if (p == kParamMelLevel) {
        a->dtc->melLevelV = a->v[kParamMelLevel] / 100.0f * 10.0f;
    } else if (p == kParamMelRandomise) {
        if (a->melRandomisePrev == 0 && a->v[kParamMelRandomise] == 1) {
            for (int i = 0; i < 16; ++i) {
                a->melMelodyLoop[i]  = (float)(lcg_next() & 0xFFFFu) / 65535.0f;
                a->melDensityLoop[i] = (float)(lcg_next() & 0xFFFFu) / 65535.0f;
            }
        }
        a->melRandomisePrev = a->v[kParamMelRandomise];
    } else if (p == kParamMelOctave) {
        updateFrequencies(a);
    } else if (p == kParamMelOSC2Partial || p == kParamMelOSC2Semi) {
        const int partial = a->v[kParamMelOSC2Partial];
        const int semi    = a->v[kParamMelOSC2Semi];
        a->melOSC2FreqScale = (float)partial * powf(2.0f, (float)semi / 12.0f);
    } else if (p == kParamMelIndex) {
        // Recompute swarm detune ratios: copy i gets (i+1)*centsPerCopy cents
        // centsPerCopy = Index/1000 * 50  (0..50 cents)
        const float centsPerCopy = (float)a->v[kParamMelIndex] / 1000.0f * 50.0f;
        for (int i = 0; i < 7; ++i)
            a->melSwarmRatio[i] = powf(2.0f, (float)(i + 1) * centsPerCopy / 1200.0f);
    }
}

// ---------------------------------------------------------------------------
// draw() — progression overview
// ---------------------------------------------------------------------------

bool draw(_NT_algorithm* self) {
    ChordsAlg* a          = (ChordsAlg*)self;
    const int progLength  = a->v[kParamProgLength];
    const int activeStep  = a->currentStep;

    const int slotW   = 32;
    const int chordY1 = 30;
    const int chordY2 = 39;
    const int octaveY = 48;
    const int frameY0 = 19;
    const int frameY1 = 50;

    for (int s = 0; s < 8; ++s) {
        if (s >= progLength) continue;

        const int rootIdx   = a->v[kParamRoot1  + s * 2];
        const int chordType = a->v[kParamChord1 + s * 2];
        const int noteIdx   = rootIdx % 12;
        const int octave    = rootIdx / 12;
        const int cx        = s * slotW + slotW / 2;
        const int col       = (s == activeStep) ? 15 : 8;

        if (s == activeStep) {
            NT_drawShapeI(kNT_box,
                          s * slotW, frameY0,
                          s * slotW + slotW - 1, frameY1,
                          8);
        }

        char combined[16];
        strcpy(combined, noteLetters[noteIdx]);
        strcat(combined, chordNames[chordType]);
        const int totalLen = (int)strlen(combined);

        int splitPos = -1;
        if (totalLen > 5) {
            for (int k = 5; k >= 1 && splitPos < 0; --k) {
                const char p = combined[k-1], c = combined[k];
                const bool pA = (p>='a'&&p<='z')||(p>='A'&&p<='Z');
                const bool cA = (c>='a'&&c<='z')||(c>='A'&&c<='Z');
                const bool pL = (p>='a'&&p<='z');
                const bool cU = (c>='A'&&c<='Z');
                if ((pA&&!cA)||(!pA&&cA)||(pL&&cU))
                    splitPos = k;
            }
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
            NT_drawText(cx, chordY1, combined, col, kNT_textCentre, kNT_textNormal);
        } else {
            char line1[8];
            memcpy(line1, combined, (size_t)splitPos);
            line1[splitPos] = '\0';
            NT_drawText(cx, chordY1, line1,            col, kNT_textCentre, kNT_textNormal);
            NT_drawText(cx, chordY2, combined+splitPos, col, kNT_textCentre, kNT_textNormal);
        }

        const char octStr[2] = { (char)('0' + octave), 0 };
        NT_drawText(cx, octaveY, octStr, col, kNT_textCentre, kNT_textNormal);
    }

    return false;
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

    // Bass bus setup
    float* bassOut = busFrames + (a->v[kParamBassOutput] - 1) * numFrames;
    const int clockBus = a->v[kParamClockIn];
    const float* clockIn = (clockBus > 0) ? busFrames + (clockBus - 1) * numFrames : nullptr;
    const int bassWave     = a->v[kParamBassWaveform];
    const int loopLength   = a->v[kParamLoopLength] > 0 ? a->v[kParamLoopLength] : 1;
    const int bassRange    = a->v[kParamBassRange];
    const int bassOffset   = a->v[kParamBassOffset];
    const int bassDensity  = a->v[kParamBassDensity];
    const float glideSecs  = a->v[kParamGlide] * 0.01f;

    // Melody bus setup
    float* melOut = busFrames + (a->v[kParamMelOutput] - 1) * numFrames;
    const int melClockBus  = a->v[kParamMelClockIn];
    const float* melClockIn = (melClockBus > 0) ? busFrames + (melClockBus - 1) * numFrames : nullptr;
    const int melLoopLength  = a->v[kParamMelLoopLength] > 0 ? a->v[kParamMelLoopLength] : 1;
    const int melRange       = a->v[kParamMelRange];
    const int melOffset      = a->v[kParamMelOffset];
    const int melDensity     = a->v[kParamMelDensity];
    const float melGlideSecs = a->v[kParamMelGlide] * 0.01f;
    const int melGlideCurve  = a->v[kParamMelGlideCurve];
    const int melNoteUpd     = a->v[kParamMelNoteUpdate];

    // Melody synth params (read once, outside hot loop)
    const int   melOSC1Wave  = a->v[kParamMelOSC1Wave];
    const int   melOSC2Wave  = a->v[kParamMelOSC2Wave];
    const int   melAlgo      = a->v[kParamMelAlgo];
    const float melIndexF    = (float)a->v[kParamMelIndex] / 1000.0f; // 0..1
    const float melOSC2Scale = a->melOSC2FreqScale;
    float swRat[7];
    for (int i = 0; i < 7; ++i) swRat[i] = a->melSwarmRatio[i];
    // Mix equal-power gains (only used when melAlgo==0, precomputed outside loop)
    const float melMixG1 = (melAlgo == 0) ? cosf(kPi * 0.5f * melIndexF) : 1.0f;
    const float melMixG2 = (melAlgo == 0) ? sinf(kPi * 0.5f * melIndexF) : 0.0f;

    // Bass DTC locals
    float bassPhase          = d->bassPhase;
    float bassFreq           = d->bassFreq;
    float bassTargetFreq     = d->bassTargetFreq;
    float bassGlideStartFreq = d->bassGlideStartFreq;
    float bassGlideProgress  = d->bassGlideProgress;
    float bassGlideDT        = d->bassGlideDT;
    float bassLevelV         = d->bassLevelV;
    float prevClockTrig      = d->prevClockTrig;
    float prevBassSample     = d->prevBassSample;
    const int glideCurve     = a->v[kParamGlideCurve];
    bool  bassGated          = a->bassGated;
    bool  bassPendingGated   = a->bassPendingGated;
    bool  bassHasPending     = a->bassHasPending;
    int   bassLoopPos        = a->bassLoopPos;
    int   bassNoteUpdateCount     = a->bassNoteUpdateCount;
    bool  bassHasPendingNote      = a->bassHasPendingNote;
    float bassNoteUpdateTargetHz  = a->bassNoteUpdateTargetHz;
    const int noteUpdate = a->v[kParamNoteUpdate];

    // Melody DTC locals
    float melPhase           = d->melPhase;
    float melFreq            = d->melFreq;
    float melTargetFreq      = d->melTargetFreq;
    float melGlideStartFreq  = d->melGlideStartFreq;
    float melGlideProgress   = d->melGlideProgress;
    float melGlideDT         = d->melGlideDT;
    float melLevelV          = d->melLevelV;
    float prevMelClockTrig   = d->prevMelClockTrig;
    float prevMelSample      = d->prevMelSample;
    float mel2Phase          = d->mel2Phase;
    float swPh[7];
    for (int i = 0; i < 7; ++i) swPh[i] = d->melSwarmPhase[i];
    bool  melGated           = a->melGated;
    bool  melPendingGated    = a->melPendingGated;
    bool  melHasPending      = a->melHasPending;
    int   melLoopPos         = a->melLoopPos;
    int   melNoteUpdateCount    = a->melNoteUpdateCount;
    bool  melHasPendingNote     = a->melHasPendingNote;
    float melNoteUpdateTargetHz = a->melNoteUpdateTargetHz;

    const int advBus = a->v[kParamAdvanceIn];
    const float* adv = (advBus > 0) ? busFrames + (advBus - 1) * numFrames : nullptr;

    const int rstBus = a->v[kParamResetIn];
    const float* rst = (rstBus > 0) ? busFrames + (rstBus - 1) * numFrames : nullptr;

    const int progLength = a->v[kParamProgLength];
    const int progDiv    = a->v[kParamProgDiv];

    float prevTrig  = d->prevAdvTrig;
    float prevReset = d->prevResetTrig;
    float ph0 = d->osc_phase[0], ph1 = d->osc_phase[1];
    float ph2 = d->osc_phase[2], ph3 = d->osc_phase[3];
    float fp0 = d->fmPhase[0],   fp1 = d->fmPhase[1];
    float fp2 = d->fmPhase[2],   fp3 = d->fmPhase[3];
    float f0  = d->osc_freq[0],  f1  = d->osc_freq[1];
    float f2  = d->osc_freq[2],  f3  = d->osc_freq[3];
    float lfoX = d->lfoX, lfoY = d->lfoY;
    const float lfoCos = a->lfoCos, lfoSin = a->lfoSin;
    float lfo2X = d->lfo2X, lfo2Y = d->lfo2Y;
    const float lfo2Cos = a->lfoCos2, lfo2Sin = a->lfoSin2;
    const float gn0 = a->osc_gainF[0], gn1 = a->osc_gainF[1];
    const float gn2 = a->osc_gainF[2], gn3 = a->osc_gainF[3];
    const float halfDepth = a->levelModDepth * 0.5f;
    const float base0 = d->osc_levelV[0] * (1.0f - halfDepth);
    const float base1 = d->osc_levelV[1] * (1.0f - halfDepth);
    const float base2 = d->osc_levelV[2] * (1.0f - halfDepth);
    const float base3 = d->osc_levelV[3] * (1.0f - halfDepth);
    const float mod0  = d->osc_levelV[0] * halfDepth;
    const float mod1  = d->osc_levelV[1] * halfDepth;
    const float mod2  = d->osc_levelV[2] * halfDepth;
    const float mod3  = d->osc_levelV[3] * halfDepth;
    const float shapeHalfDepth = a->shapeModDepth * 0.5f;
    const float sgBase0 = gn0 * (1.0f - shapeHalfDepth);
    const float sgBase1 = gn1 * (1.0f - shapeHalfDepth);
    const float sgBase2 = gn2 * (1.0f - shapeHalfDepth);
    const float sgBase3 = gn3 * (1.0f - shapeHalfDepth);
    const float sgMod0  = gn0 * shapeHalfDepth;
    const float sgMod1  = gn1 * shapeHalfDepth;
    const float sgMod2  = gn2 * shapeHalfDepth;
    const float sgMod3  = gn3 * shapeHalfDepth;
    const float fmFreqMult = a->fmFreqMult;
    const float fmDepth    = a->fmDepth;
    const bool  fmPost     = (a->v[kParamFMPrePost] != 0);
    const float ps    = a->phaseStep;
    const float phOff0 = 0.0f;
    const float fpOff0 = ps;
    const float phOff1 = ps * 2.0f;
    const float fpOff1 = ps * 3.0f;
    const float phOff2 = ps * 4.0f;
    const float fpOff2 = ps * 5.0f;
    const float phOff3 = ps * 6.0f;
    const float fpOff3 = ps * 7.0f;
    float bdt[4] = { f0/sr, f1/sr, f2/sr, f3/sr };

    for (int i = 0; i < numFrames; ++i) {
        // ResetIn rising edge
        if (rst) {
            const float r = rst[i];
            if (prevReset < 1.0f && r >= 1.0f) {
                a->currentStep = 0;
                updateFrequencies(a);
                f0 = d->osc_freq[0]; f1 = d->osc_freq[1];
                f2 = d->osc_freq[2]; f3 = d->osc_freq[3];
                bdt[0] = f0/sr; bdt[1] = f1/sr; bdt[2] = f2/sr; bdt[3] = f3/sr;
            }
            prevReset = r;
        }

        // AdvanceIn rising edge
        if (adv) {
            const float t = adv[i];
            if (prevTrig < 1.0f && t >= 1.0f) {
                if (++a->advDivCount >= progDiv) {
                    a->advDivCount = 0;
                    a->currentStep = (a->currentStep + 1) % progLength;
                    updateFrequencies(a);
                    f0 = d->osc_freq[0]; f1 = d->osc_freq[1];
                    f2 = d->osc_freq[2]; f3 = d->osc_freq[3];
                    bdt[0] = f0/sr; bdt[1] = f1/sr; bdt[2] = f2/sr; bdt[3] = f3/sr;
                }
            }
            prevTrig = t;
        }

        // LFO1 — level modulation
        const float nx = lfoCos * lfoX - lfoSin * lfoY;
        const float ny = lfoSin * lfoX + lfoCos * lfoY;
        lfoX = nx; lfoY = ny;
        const float lv0 = base0 + mod0 * lfoY;
        const float lv1 = base1 + mod1 * lfoX;
        const float lv2 = base2 - mod2 * lfoY;
        const float lv3 = base3 - mod3 * lfoX;

        // LFO2 — shape modulation
        const float n2x = lfo2Cos * lfo2X - lfo2Sin * lfo2Y;
        const float n2y = lfo2Sin * lfo2X + lfo2Cos * lfo2Y;
        lfo2X = n2x; lfo2Y = n2y;
        const float eg0 = sgBase0 + sgMod0 * lfo2Y;
        const float eg1 = sgBase1 + sgMod1 * lfo2X;
        const float eg2 = sgBase2 - sgMod2 * lfo2Y;
        const float eg3 = sgBase3 - sgMod3 * lfo2X;

        // ---- Bass oscillator ----
        if (clockIn) {
            const float clk = clockIn[i];
            if (prevClockTrig < 1.0f && clk >= 1.0f) {
                bassLoopPos = (bassLoopPos + 1) % loopLength;

                if (!bassGated) {
                    const float mval = a->melodyLoop[bassLoopPos];
                    int noteIdx = bassOffset + (int)(mval * (float)(bassRange + 1));
                    if (noteIdx > 15) noteIdx = 15;
                    if (noteIdx < 0)  noteIdx = 0;
                    const float tgtHz = a->notePool[noteIdx];
                    bassTargetFreq = tgtHz;
                    if (glideSecs <= 0.0f) {
                        bassFreq = tgtHz;
                        bassGlideProgress = 1.0f;
                        bassGlideDT = 0.0f;
                    } else {
                        bassGlideStartFreq = bassFreq;
                        bassGlideProgress  = 0.0f;
                        const float samps  = glideSecs * sr;
                        bassGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
                    }
                } else if (noteUpdate > 0) {
                    ++bassNoteUpdateCount;
                    if (bassNoteUpdateCount >= noteUpdate) {
                        bassNoteUpdateCount = 0;
                        const float mval = a->melodyLoop[bassLoopPos];
                        int noteIdx = bassOffset + (int)(mval * (float)(bassRange + 1));
                        if (noteIdx > 15) noteIdx = 15;
                        if (noteIdx < 0)  noteIdx = 0;
                        bassNoteUpdateTargetHz = a->notePool[noteIdx];
                        bassHasPendingNote = true;
                    }
                }

                const bool newGate = (a->densityLoop[bassLoopPos] < (float)bassDensity / 100.0f);
                if (newGate != bassGated) {
                    bassPendingGated = newGate;
                    bassHasPending = true;
                }
            }
            prevClockTrig = clk;
        }

        // Bass glide
        if (bassGlideProgress < 1.0f) {
            bassGlideProgress += bassGlideDT;
            if (bassGlideProgress >= 1.0f) {
                bassFreq = bassTargetFreq;
                bassGlideProgress = 1.0f;
            } else {
                const float t  = bassGlideProgress;
                const float tc = glideCurve * 0.01f;
                float t_shaped;
                if (tc <= 0.5f) {
                    const float a_  = tc * 2.0f;
                    const float tFast = t * (2.0f - t);
                    t_shaped = tFast + a_ * (t - tFast);
                } else {
                    const float a_  = (tc - 0.5f) * 2.0f;
                    const float tSlow = t * t;
                    t_shaped = t + a_ * (tSlow - t);
                }
                bassFreq = bassGlideStartFreq + (bassTargetFreq - bassGlideStartFreq) * t_shaped;
            }
        }

        // Bass waveform
        float bassSample;
        if (bassWave == 0) {
            bassSample = sinf(2.0f * kPi * bassPhase);
        } else {
            bassSample = (bassPhase < 0.25f) ? bassPhase * 4.0f
                       : (bassPhase < 0.75f) ? 2.0f - bassPhase * 4.0f
                                             : (bassPhase - 1.0f) * 4.0f;
        }

        // Bass zero-crossing: apply pending gate / note update
        if ((bassHasPending || bassHasPendingNote) && (prevBassSample * bassSample <= 0.0f)) {
            if (bassHasPending) {
                if (!bassGated && bassPendingGated)
                    bassNoteUpdateCount = 0;
                bassGated = bassPendingGated;
                bassHasPending = false;
            }
            if (bassHasPendingNote) {
                if (glideSecs <= 0.0f || bassFreq == bassNoteUpdateTargetHz) {
                    bassFreq = bassNoteUpdateTargetHz;
                    bassTargetFreq = bassNoteUpdateTargetHz;
                    bassGlideProgress = 1.0f;
                    bassGlideDT = 0.0f;
                } else {
                    bassTargetFreq     = bassNoteUpdateTargetHz;
                    bassGlideStartFreq = bassFreq;
                    bassGlideProgress  = 0.0f;
                    const float samps  = glideSecs * sr;
                    bassGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
                }
                bassHasPendingNote = false;
            }
        }
        prevBassSample = bassSample;
        bassOut[i] += bassGated ? bassSample * bassLevelV : 0.0f;
        bassPhase += bassFreq / sr;
        if (bassPhase >= 1.0f) bassPhase -= 1.0f;
        // ---- End bass oscillator ----

        // ---- Melody oscillator ----
        if (melClockIn) {
            const float clk = melClockIn[i];
            if (prevMelClockTrig < 1.0f && clk >= 1.0f) {
                melLoopPos = (melLoopPos + 1) % melLoopLength;
                if (!melGated) {
                    const float mval = a->melMelodyLoop[melLoopPos];
                    int noteIdx = melOffset + (int)(mval * (float)(melRange + 1));
                    if (noteIdx > 15) noteIdx = 15;
                    if (noteIdx < 0)  noteIdx = 0;
                    const float tgtHz = a->melNotePool[noteIdx];
                    melTargetFreq = tgtHz;
                    if (melGlideSecs <= 0.0f) {
                        melFreq = tgtHz;
                        melGlideProgress = 1.0f;
                        melGlideDT = 0.0f;
                    } else {
                        melGlideStartFreq = melFreq;
                        melGlideProgress  = 0.0f;
                        const float samps = melGlideSecs * sr;
                        melGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
                    }
                } else if (melNoteUpd > 0) {
                    ++melNoteUpdateCount;
                    if (melNoteUpdateCount >= melNoteUpd) {
                        melNoteUpdateCount = 0;
                        const float mval = a->melMelodyLoop[melLoopPos];
                        int noteIdx = melOffset + (int)(mval * (float)(melRange + 1));
                        if (noteIdx > 15) noteIdx = 15;
                        if (noteIdx < 0)  noteIdx = 0;
                        melNoteUpdateTargetHz = a->melNotePool[noteIdx];
                        melHasPendingNote = true;
                    }
                }
                const bool newGate = (a->melDensityLoop[melLoopPos] < (float)melDensity / 100.0f);
                if (newGate != melGated) {
                    melPendingGated = newGate;
                    melHasPending = true;
                }
            }
            prevMelClockTrig = clk;
        }

        // Melody glide
        if (melGlideProgress < 1.0f) {
            melGlideProgress += melGlideDT;
            if (melGlideProgress >= 1.0f) {
                melFreq = melTargetFreq;
                melGlideProgress = 1.0f;
            } else {
                const float t  = melGlideProgress;
                const float tc = melGlideCurve * 0.01f;
                float t_shaped;
                if (tc <= 0.5f) {
                    const float a_    = tc * 2.0f;
                    const float tFast = t * (2.0f - t);
                    t_shaped = tFast + a_ * (t - tFast);
                } else {
                    const float a_    = (tc - 0.5f) * 2.0f;
                    const float tSlow = t * t;
                    t_shaped = t + a_ * (tSlow - t);
                }
                melFreq = melGlideStartFreq + (melTargetFreq - melGlideStartFreq) * t_shaped;
            }
        }

        // OSC1 sample (used for zero-crossing detection and as base for most algos)
        const float melDt = melFreq / sr;
        float melCoreSample = oscWave(melOSC1Wave, melPhase, melDt);

        // Zero-crossing: apply pending melody gate / note update
        if ((melHasPending || melHasPendingNote) && (prevMelSample * melCoreSample <= 0.0f)) {
            if (melHasPending) {
                if (!melGated && melPendingGated)
                    melNoteUpdateCount = 0;
                melGated = melPendingGated;
                melHasPending = false;
            }
            if (melHasPendingNote) {
                if (melGlideSecs <= 0.0f || melFreq == melNoteUpdateTargetHz) {
                    melFreq = melNoteUpdateTargetHz;
                    melTargetFreq = melNoteUpdateTargetHz;
                    melGlideProgress = 1.0f;
                    melGlideDT = 0.0f;
                } else {
                    melTargetFreq     = melNoteUpdateTargetHz;
                    melGlideStartFreq = melFreq;
                    melGlideProgress  = 0.0f;
                    const float samps = melGlideSecs * sr;
                    melGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
                }
                melHasPendingNote = false;
            }
        }
        prevMelSample = melCoreSample;

        // Algo-based synthesis
        float melSample;
        float melPhaseAdv = melDt; // default; FM overrides this

        if (melGated) {
            if (melAlgo == 2) {
                // Swarm: OSC1 + 7 detuned copies of OSC1's waveform, normalised to 1/8
                float sum = melCoreSample;
                for (int ci = 0; ci < 7; ++ci) {
                    const float swDt = melFreq * swRat[ci] / sr;
                    sum += oscWave(melOSC1Wave, swPh[ci], swDt);
                    swPh[ci] += swDt;
                    swPh[ci] -= floorf(swPh[ci]);
                }
                melSample = sum * (1.0f / 8.0f);
            } else {
                // All other algos use OSC2
                const float dt2  = melFreq * melOSC2Scale / sr;
                const float osc2 = (dt2 > 0.0f) ? oscWave(melOSC2Wave, mel2Phase, dt2) : 0.0f;
                mel2Phase += dt2;
                mel2Phase -= floorf(mel2Phase);

                switch (melAlgo) {
                case 0: // Mix — equal-power crossfade
                    melSample = melCoreSample * melMixG1 + osc2 * melMixG2;
                    break;
                case 1: { // FM — linear FM: carrier phase advances at modulated rate
                    // fmDt = melDt * (1 + osc2 * index * 10): at index=1 and osc2=±1, ±10× freq swing
                    const float fmDt = melDt * (1.0f + osc2 * melIndexF * 10.0f);
                    melSample = oscWave(melOSC1Wave, melPhase, fmDt);
                    melPhaseAdv = fmDt; // accumulate FM into carrier phase
                    break;
                }
                case 3: { // PM — phase modulation: OSC2 offsets carrier phase each sample
                    // pmOff in cycles: at index=1 and osc2=±1, ±5 cycle offset
                    const float pmOff = osc2 * melIndexF * 5.0f;
                    float pmPh = melPhase + pmOff;
                    pmPh -= floorf(pmPh);
                    melSample = oscWave(melOSC1Wave, pmPh, melDt);
                    break;
                }
                default: // AM — OSC2 modulates OSC1 amplitude
                    // output = OSC1 * (1 + depth * OSC2); at depth=1, osc2=1: 2× amplitude
                    melSample = melCoreSample * (1.0f + osc2 * melIndexF);
                    break;
                }
            }
        } else {
            melSample = 0.0f;
        }

        melOut[i] += melGated ? melSample * melLevelV : 0.0f;

        // Advance melody carrier phase (FM uses modulated rate; all others use base melDt)
        melPhase += melPhaseAdv;
        melPhase -= floorf(melPhase);
        // ---- End melody oscillator ----

        // FM modulator signals for chord oscillators
        const float m0 = fmDepth * sinf(2.0f * kPi * (fp0 + fpOff0));
        const float m1 = fmDepth * sinf(2.0f * kPi * (fp1 + fpOff1));
        const float m2 = fmDepth * sinf(2.0f * kPi * (fp2 + fpOff2));
        const float m3 = fmDepth * sinf(2.0f * kPi * (fp3 + fpOff3));
        float w0, w1, w2, w3;
        if (fmPost) {
            w0 = wfold(sinf(2.0f * kPi * (ph0 + phOff0)) * eg0 + m0);
            w1 = wfold(sinf(2.0f * kPi * (ph1 + phOff1)) * eg1 + m1);
            w2 = wfold(sinf(2.0f * kPi * (ph2 + phOff2)) * eg2 + m2);
            w3 = wfold(sinf(2.0f * kPi * (ph3 + phOff3)) * eg3 + m3);
        } else {
            w0 = wfold(sinf(2.0f * kPi * (ph0 + phOff0) + m0) * eg0);
            w1 = wfold(sinf(2.0f * kPi * (ph1 + phOff1) + m1) * eg1);
            w2 = wfold(sinf(2.0f * kPi * (ph2 + phOff2) + m2) * eg2);
            w3 = wfold(sinf(2.0f * kPi * (ph3 + phOff3) + m3) * eg3);
        }

        out[i] = w0*lv0 + w1*lv1 + w2*lv2 + w3*lv3;

        ph0 += bdt[0]; if (ph0 >= 1.0f) ph0 -= 1.0f;
        ph1 += bdt[1]; if (ph1 >= 1.0f) ph1 -= 1.0f;
        ph2 += bdt[2]; if (ph2 >= 1.0f) ph2 -= 1.0f;
        ph3 += bdt[3]; if (ph3 >= 1.0f) ph3 -= 1.0f;

        fp0 += bdt[0] * fmFreqMult; if (fp0 >= 1.0f) fp0 -= 1.0f; else if (fp0 < 0.0f) fp0 += 1.0f;
        fp1 += bdt[1] * fmFreqMult; if (fp1 >= 1.0f) fp1 -= 1.0f; else if (fp1 < 0.0f) fp1 += 1.0f;
        fp2 += bdt[2] * fmFreqMult; if (fp2 >= 1.0f) fp2 -= 1.0f; else if (fp2 < 0.0f) fp2 += 1.0f;
        fp3 += bdt[3] * fmFreqMult; if (fp3 >= 1.0f) fp3 -= 1.0f; else if (fp3 < 0.0f) fp3 += 1.0f;
    }

    d->prevAdvTrig   = prevTrig;
    d->prevResetTrig = prevReset;
    d->lfoX = lfoX;   d->lfoY = lfoY;
    d->lfo2X = lfo2X; d->lfo2Y = lfo2Y;
    d->osc_phase[0] = ph0; d->osc_phase[1] = ph1;
    d->osc_phase[2] = ph2; d->osc_phase[3] = ph3;
    d->fmPhase[0]   = fp0; d->fmPhase[1]   = fp1;
    d->fmPhase[2]   = fp2; d->fmPhase[3]   = fp3;
    d->osc_freq[0]  = f0;  d->osc_freq[1]  = f1;
    d->osc_freq[2]  = f2;  d->osc_freq[3]  = f3;
    // Bass write-back
    d->bassPhase          = bassPhase;
    d->bassFreq           = bassFreq;
    d->bassTargetFreq     = bassTargetFreq;
    d->bassGlideStartFreq = bassGlideStartFreq;
    d->bassGlideProgress  = bassGlideProgress;
    d->bassGlideDT        = bassGlideDT;
    d->prevClockTrig      = prevClockTrig;
    d->prevBassSample     = prevBassSample;
    a->bassGated               = bassGated;
    a->bassPendingGated        = bassPendingGated;
    a->bassHasPending          = bassHasPending;
    a->bassLoopPos             = bassLoopPos;
    a->bassNoteUpdateCount     = bassNoteUpdateCount;
    a->bassHasPendingNote      = bassHasPendingNote;
    a->bassNoteUpdateTargetHz  = bassNoteUpdateTargetHz;
    // Melody write-back
    d->melPhase           = melPhase;
    d->melFreq            = melFreq;
    d->melTargetFreq      = melTargetFreq;
    d->melGlideStartFreq  = melGlideStartFreq;
    d->melGlideProgress   = melGlideProgress;
    d->melGlideDT         = melGlideDT;
    d->prevMelClockTrig   = prevMelClockTrig;
    d->prevMelSample      = prevMelSample;
    d->mel2Phase          = mel2Phase;
    for (int i = 0; i < 7; ++i) d->melSwarmPhase[i] = swPh[i];
    a->melGated              = melGated;
    a->melPendingGated       = melPendingGated;
    a->melHasPending         = melHasPending;
    a->melLoopPos            = melLoopPos;
    a->melNoteUpdateCount    = melNoteUpdateCount;
    a->melHasPendingNote     = melHasPendingNote;
    a->melNoteUpdateTargetHz = melNoteUpdateTargetHz;
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
