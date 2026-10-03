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
// Triads/sus: voice 4 (index 3) = −12 (root one octave below OSC1).
// 7th/extended chords: all 4 distinct chord tones (5th omitted where needed).
static const int8_t chordIntervals[17][4] = {
    {  0,  0,  0,   0 }, //  0 uni   (unisono — all voices at root)
    {  0,  4,  7, -12 }, //  1 maj
    {  0,  3,  7, -12 }, //  2 min
    {  0,  4,  7,  10 }, //  3 7     (dominant 7th)
    {  0,  4,  7,  11 }, //  4 maj7
    {  0,  3,  7,  10 }, //  5 min7
    {  0,  2,  7, -12 }, //  6 sus2
    {  0,  5,  7, -12 }, //  7 sus4
    {  0,  6, 12,  18 }, //  8 tri   (stacked tritones)
    {  0,  4,  9,  14 }, //  9 6/9   (R M3 M6 M9, omits 5th)
    {  0,  3,  6,  10 }, // 10 m7b5  (half-dim: R m3 d5 m7)
    {  0,  3,  6,   9 }, // 11 dim7  (R m3 d5 d7)
    {  0,  4, 10,  14 }, // 12 9     (dominant 9: R M3 m7 M9, omits 5th)
    {  0,  4, 11,  14 }, // 13 maj9  (R M3 M7 M9, omits 5th)
    {  0,  3, 10,  14 }, // 14 min9  (R m3 m7 M9, omits 5th)
    {  0,  3,  7,   9 }, // 15 min6  (R m3 P5 M6)
    {  0,  5,  7,  10 }, // 16 7sus4 (R P4 P5 m7)
};

// C0 reference: C3 (130.8128 Hz) is 3 octaves above C0
static const float kC0Hz = 130.8128f / 8.0f; // 16.3516 Hz
static const float kPi   = 3.14159265358979f;

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
    kParamBassRange,    // 43  (was MelodyRange)
    kParamBassOffset,   // 44  (was MelodyOffset)
    kParamRandomise,    // 45
    kParamLoopLength,   // 46
    kParamGlide,        // 47
    kParamBassDensity,  // 48
    kParamGlideCurve,   // 49
    kParamNoteUpdate,   // 50
    // Melody page
    kParamMelWaveform,  // 51
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
    // Melody timbre
    kParamMelOctave,    // 63
    kParamMelFold,      // 64
    kParamMelCutoff,    // 65
    kParamMelFeedback,  // 66
    kNumParams          // 67
};

#define ROOT_PARAM(n) \
    { .name="Root " #n,  .min=0, .max=60, .def=36, \
      .unit=kNT_unitEnum, .scaling=0, .enumStrings=rootNames },
#define CHORD_PARAM(n) \
    { .name="Chord " #n, .min=0, .max=16, .def=1, \
      .unit=kNT_unitEnum, .scaling=0, .enumStrings=chordNames },
static const char* const fmPrePostNames[]    = { "pre", "post", NULL };
static const char* const bassWaveformNames[] = { "sine", "tri",  NULL };
// randomiseNames removed — Randomise uses kNT_unitNone (displays 0/1)

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
    // Melody page (index 51+)
    { .name="MelWave",     .min=0,  .max=1,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=bassWaveformNames },
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
    // Melody timbre
    { .name="MelOctave",   .min=-4, .max=4,    .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="Fold",        .min=0,  .max=1000, .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="Cutoff",      .min=20, .max=20000, .def=1000,
      .unit=kNT_unitHz,      .scaling=0,               .enumStrings=NULL },
    { .name="Feedback",    .min=-100,.max=100,  .def=0,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
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
    kParamMelWaveform,
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
    kParamMelFold,
    kParamMelCutoff,
    kParamMelFeedback,
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
    float osc_phase[4];   // carrier phases
    float fmPhase[4];     // modulator phases, advance at carrier freq * fmFreqMult
    float osc_freq[4];
    float osc_levelV[4]; // peak amplitude in volts (0..10)
    float prevAdvTrig;
    float prevResetTrig;
    float lfoX;   // quadrature LFO1 state ≈ cos(θ)
    float lfoY;   // quadrature LFO1 state ≈ sin(θ)
    float lfo2X;  // quadrature LFO2 state ≈ cos(θ)
    float lfo2Y;  // quadrature LFO2 state ≈ sin(θ)
    // Bass oscillator
    float bassPhase;
    float bassFreq;           // current (gliding) frequency
    float bassTargetFreq;     // glide target
    float bassGlideStartFreq; // frequency at glide start
    float bassGlideProgress;  // normalised 0→1; 1.0 = no active glide
    float bassGlideDT;        // per-sample progress increment
    float bassLevelV;         // peak amplitude in volts
    float prevClockTrig;
    float prevBassSample;     // previous bass output sample (zero-crossing detection)
    // Melody oscillator
    float melPhase;
    float melFreq;            // current (gliding) frequency
    float melTargetFreq;
    float melGlideStartFreq;
    float melGlideProgress;   // 1.0 = no active glide
    float melGlideDT;
    float melLevelV;
    float prevMelClockTrig;
    float prevMelSample;      // for zero-crossing detection
    float melBpfS1, melBpfS2; // biquad bandpass filter state (Direct Form II Transposed)
    float prevBpfOut;         // BPF output from previous sample (1-sample feedback delay)
};

struct ChordsAlg : public _NT_algorithm {
    ChordsAlg_DTC* dtc;
    int currentStep;
    int   advDivCount;    // counts incoming advance triggers; advances step every v[kParamProgDiv] triggers
    float lfoCos;          // LFO1 rotation matrix: cos(2π*f/fs)
    float lfoSin;          // LFO1 rotation matrix: sin(2π*f/fs)
    float levelModDepth;   // 0..1, LevelMod/100
    float lfoCos2;         // LFO2 rotation matrix: cos(2π*f/fs)
    float lfoSin2;         // LFO2 rotation matrix: sin(2π*f/fs)
    float shapeModDepth;   // 0..1, ShapeMod/100
    float osc_gainF[4];    // wavefold drive [1.0, 9.0]: 1.0=sine, 9.0=4 folds
    float fmFreqMult;      // FMFreq parameter as float (1..16)
    float fmDepth;         // modulation index in radians (FMDepth * 0.1)
    float phaseStep;       // Phase/360: interleaved offset step (0..1 normalized)
    // Bass
    float   notePool[16];     // sorted ascending frequencies (4 chord notes × 4 octaves: −2..+1)
    float   melodyLoop[16];   // random melody values [0,1], select note from pool window
    float   densityLoop[16];  // random density values [0,1], gate threshold comparison
    int     bassLoopPos;      // current loop step (0..loopLength-1)
    bool    bassGated;        // current gate state (playing or silent)
    bool    bassPendingGated; // pending gate change (applied at next zero crossing)
    bool    bassHasPending;   // a gate change is waiting for a zero crossing
    int16_t bassRandomisePrev;// last seen value of kParamRandomise (edge detection)
    int     bassNoteUpdateCount;    // clock ticks since last note-update while gated
    bool    bassHasPendingNote;     // a NoteUpdate pitch change awaits zero crossing
    float   bassNoteUpdateTargetHz; // pending target frequency for NoteUpdate
    // Melody sequencer
    float   melNotePool[16];        // sorted ascending, 4 notes × 4 octaves (+2..+5)
    float   melMelodyLoop[16];      // random melody values [0,1]
    float   melDensityLoop[16];     // random density values [0,1]
    int     melLoopPos;
    bool    melGated;
    bool    melPendingGated;
    bool    melHasPending;
    int16_t melRandomisePrev;
    int     melNoteUpdateCount;
    bool    melHasPendingNote;
    float   melNoteUpdateTargetHz;
    // Melody timbre cached params (updated in parameterChanged)
    float   melOctaveShift; // 2^MelOctave (applied when building note pool)
    float   melFoldSg;      // per-stage wavefold gain: 1.0 + Fold/1000
    float   melBpfB0;       // biquad BPF b0/a0
    float   melBpfA1;       // biquad BPF a1/a0
    float   melBpfA2;       // biquad BPF a2/a0
    float   melFeedback;    // Feedback/100 (-1..+1)

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
                                   melFoldSg(1.0f), melBpfB0(0.0f),
                                   melBpfA1(0.0f), melBpfA2(0.0f), melFeedback(0.0f) {
        osc_gainF[0] = osc_gainF[1] = osc_gainF[2] = osc_gainF[3] = 1.0f;
        for (int i = 0; i < 16; ++i) notePool[i] = 0.0f;
        for (int i = 0; i < 16; ++i) { melodyLoop[i] = 0.5f; densityLoop[i] = 0.5f; }
        for (int i = 0; i < 16; ++i) melNotePool[i] = 0.0f;
        for (int i = 0; i < 16; ++i) { melMelodyLoop[i] = 0.5f; melDensityLoop[i] = 0.5f; }
    }
};

// ---------------------------------------------------------------------------
// DSP helpers
// ---------------------------------------------------------------------------

// Simple LCG random number generator (file-scope state shared across all instances).
static uint32_t lcg_state = 0x12345678u;
static inline uint32_t lcg_next() {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

// Wavefolder: reflects x into [-1, 1] via triangle wave (branchless).
// gain=1.0 → sine (peaks just touch ±1). gain=9.0 → 4 folds.
static inline float wfold(float x) {
    x += 1.0f;
    float t = x - 4.0f * floorf(x * 0.25f); // t ∈ [0, 4)
    return (t < 2.0f) ? (t - 1.0f) : (3.0f - t);
}

// Build the 16-note bass pool from current chord (4 notes × 4 octaves: −2..+1),
// sorted ascending. Called whenever chord/root/octave changes.
static void updateNotePool(ChordsAlg* a) {
    int step      = a->currentStep;
    int rootIdx   = a->v[kParamRoot1  + step * 2];
    int chordType = a->v[kParamChord1 + step * 2];
    // Bass pool is independent of the chord Octave shift — always at rootIdx pitch class.
    float rootHz  = kC0Hz * powf(2.0f, (float)rootIdx / 12.0f);
    int np = 0;
    for (int i = 0; i < 4; ++i) {
        const float baseHz = rootHz * powf(2.0f, (float)chordIntervals[chordType][i] / 12.0f);
        for (int oct = -2; oct <= 1; ++oct)
            a->notePool[np++] = baseHz * powf(2.0f, (float)oct);
    }
    // Insertion sort ascending (16 elements)
    for (int i = 1; i < 16; ++i) {
        const float key = a->notePool[i];
        int j = i - 1;
        while (j >= 0 && a->notePool[j] > key) { a->notePool[j+1] = a->notePool[j]; --j; }
        a->notePool[j+1] = key;
    }
}

// Build the 16-note melody pool from current chord (4 notes × 4 octaves: base+2..base+5).
// MelOctave shifts the entire range up/down by octaves (-4..+4).
static void updateMelNotePool(ChordsAlg* a) {
    int step      = a->currentStep;
    int rootIdx   = a->v[kParamRoot1  + step * 2];
    int chordType = a->v[kParamChord1 + step * 2];
    float rootHz  = kC0Hz * powf(2.0f, (float)rootIdx / 12.0f);
    const int octOff = a->v[kParamMelOctave]; // -4..+4; 0 = default (+2..+5)
    // Update cached octave shift multiplier while we're here
    a->melOctaveShift = powf(2.0f, (float)octOff);
    int np = 0;
    for (int i = 0; i < 4; ++i) {
        const float baseHz = rootHz * powf(2.0f, (float)chordIntervals[chordType][i] / 12.0f);
        for (int oct = 2 + octOff; oct <= 5 + octOff; ++oct)
            a->melNotePool[np++] = baseHz * powf(2.0f, (float)oct);
    }
    // Insertion sort ascending
    for (int i = 1; i < 16; ++i) {
        const float key = a->melNotePool[i];
        int j = i - 1;
        while (j >= 0 && a->melNotePool[j] > key) { a->melNotePool[j+1] = a->melNotePool[j]; --j; }
        a->melNotePool[j+1] = key;
    }
}

// Called only on chord/root/step change — powf calls are acceptable here.
static void updateFrequencies(ChordsAlg* a) {
    ChordsAlg_DTC* d = a->dtc;
    int step      = a->currentStep;
    int rootIdx   = a->v[kParamRoot1  + step * 2]; // 0..60, C0..C5
    int chordType = a->v[kParamChord1 + step * 2];
    float rootHz  = kC0Hz * powf(2.0f, (float)rootIdx / 12.0f + (float)a->v[kParamOctave]);
    const int detuneCents = a->v[kParamDetune];
    for (int i = 0; i < 4; ++i) {
        const float baseHz = rootHz * powf(2.0f, (float)chordIntervals[chordType][i] / 12.0f);
        d->osc_freq[i] = (i == 0) ? baseHz
                                   : baseHz * powf(2.0f, (float)(i * detuneCents) / 1200.0f);
    }
    updateNotePool(a);

    // On chord change, move bass to nearest in-chord note, using glide if set.
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

    // Rebuild melody pool and snap melody to nearest in-chord note.
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

    // Initialise to C3 major (default root=36=C3, chord=0=maj).
    // Intervals: [0, 4, 7, -12]. Cannot call updateFrequencies() here
    // because v[] is not yet populated during construct().
    static const float kMajIntervals[4] = { 0.0f, 4.0f, 7.0f, -12.0f };
    // rootIdx=36 → kC0Hz * 2^(36/12) = kC0Hz * 8 = kC3Hz
    const float kC3Hz = kC0Hz * 8.0f;
    for (int i = 0; i < 4; ++i) {
        d->osc_phase[i]  = 0.0f;
        d->fmPhase[i]    = 0.0f;
        d->osc_freq[i]   = kC3Hz * powf(2.0f, kMajIntervals[i] / 12.0f);
        d->osc_levelV[i] = 2.5f; // matches def=25 → 25/100*10 = 2.5V
    }
    d->prevAdvTrig   = 0.0f;
    d->prevResetTrig = 0.0f;
    d->lfoX          = 1.0f; // cosine starts at peak
    d->lfoY          = 0.0f;
    d->lfo2X         = 1.0f;
    d->lfo2Y         = 0.0f;
    d->bassPhase          = 0.0f;
    d->bassFreq           = kC0Hz * 4.0f;  // C2
    d->bassTargetFreq     = d->bassFreq;
    d->bassGlideStartFreq = d->bassFreq;
    d->bassGlideProgress  = 1.0f;   // 1.0 = no active glide
    d->bassGlideDT        = 0.0f;
    d->bassLevelV         = 5.0f;   // def=50 → 50/100*10=5V
    d->prevClockTrig      = 0.0f;
    d->prevBassSample     = 0.0f;
    // Melody oscillator — default to a mid-high note (C5 ≈ rootHz * 2^2 at C3)
    d->melPhase           = 0.0f;
    d->melFreq            = kC0Hz * 32.0f;  // C5
    d->melTargetFreq      = d->melFreq;
    d->melGlideStartFreq  = d->melFreq;
    d->melGlideProgress   = 1.0f;
    d->melGlideDT         = 0.0f;
    d->melLevelV          = 5.0f;
    d->prevMelClockTrig   = 0.0f;
    d->prevMelSample      = 0.0f;
    d->melBpfS1           = 0.0f;
    d->melBpfS2           = 0.0f;
    d->prevBpfOut         = 0.0f;

    ChordsAlg* a = new (ptrs.sram) ChordsAlg(d);
    a->parameters     = parameters;
    a->parameterPages = &paramPages;
    // Default LevelFreq=10 → 1.0 Hz, ShapeFreq=10 → 1.0 Hz; depths=0 (no modulation)
    a->lfoCos        = cosf(2.0f * kPi * 1.0f / NT_globals.sampleRate);
    a->lfoSin        = sinf(2.0f * kPi * 1.0f / NT_globals.sampleRate);
    a->levelModDepth = 0.0f;
    a->lfoCos2       = cosf(2.0f * kPi * 1.0f / NT_globals.sampleRate);
    a->lfoSin2       = sinf(2.0f * kPi * 1.0f / NT_globals.sampleRate);
    a->shapeModDepth = 0.0f;
    for (int i = 0; i < 4; ++i)
        a->osc_gainF[i] = 1.0f + a->v[kParamOSC1Shape + i*2] * 8.0f / 1000.0f;
    // Init default note pool: C3 major (rootIdx=36, chordType=0=maj, octave=0)
    // intervals: 0, 4, 7, -12 → C3, E3, G3, C2 — 4 notes × 4 octaves (−2..+1)
    {
        const float kC3Hz_ = kC0Hz * 8.0f; // 2^(36/12) = 8
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
        // Default bass frequency to lowest pool note
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
        d->melFreq           = a->melNotePool[8]; // start mid-pool
        d->melTargetFreq     = d->melFreq;
        d->melGlideStartFreq = d->melFreq;
    }
    // Init BPF coefficients for default Cutoff=1000 Hz (def=10000, scaling10)
    {
        // Q for 7-semitone bandwidth: 1/(2*sinh(ln2*7/24)) ≈ 2.453
        static const float kMelBpfQ = 2.453f;
        const float freq  = 1000.0f;
        const float w0    = 2.0f * kPi * freq / NT_globals.sampleRate;
        const float alpha = sinf(w0) / (2.0f * kMelBpfQ);
        const float ia0   = 1.0f / (1.0f + alpha);
        a->melBpfB0 =  alpha * ia0;
        a->melBpfA1 = -2.0f * cosf(w0) * ia0;
        a->melBpfA2 = (1.0f - alpha) * ia0;
    }
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
        a->fmDepth = a->v[kParamFMDepth] * 0.01f;   // 0..1000 → 0..10 radians
    } else if (p == kParamPhase) {
        a->phaseStep = a->v[kParamPhase] / 3600.0f; // 0..3600 (×0.1°) → 0..1
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
        updateFrequencies(a); // rebuilds melody pool with new octave offset
    } else if (p == kParamMelFold) {
        a->melFoldSg = 1.0f + (float)a->v[kParamMelFold] / 1000.0f; // 1.0..2.0 per stage
    } else if (p == kParamMelCutoff) {
        // Recompute biquad BPF coefficients. Q≈2.453 gives 7-semitone bandwidth at 12dB/oct.
        static const float kMelBpfQ = 2.453f;
        const float freq  = (float)a->v[kParamMelCutoff]; // raw Hz (20..20000)
        const float w0    = 2.0f * kPi * freq / NT_globals.sampleRate;
        const float alpha = sinf(w0) / (2.0f * kMelBpfQ);
        const float ia0   = 1.0f / (1.0f + alpha);
        a->melBpfB0 =  alpha * ia0;
        a->melBpfA1 = -2.0f * cosf(w0) * ia0;
        a->melBpfA2 = (1.0f - alpha) * ia0;
    } else if (p == kParamMelFeedback) {
        a->melFeedback = (float)a->v[kParamMelFeedback] / 100.0f;
    }
    // Bus/routing/sequencer params for both pages read live from v[] in step().
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
    const int frameY0 = 19;
    const int frameY1 = 50;

    for (int s = 0; s < 8; ++s) {
        if (s >= progLength) continue;

        const int rootIdx   = a->v[kParamRoot1  + s * 2];
        const int chordType = a->v[kParamChord1 + s * 2];
        const int noteIdx   = rootIdx % 12;
        const int octave    = rootIdx / 12;
        const int cx        = s * slotW + slotW / 2; // centre x of slot
        const int col       = (s == activeStep) ? 15 : 8;

        // Active chord: draw unfilled bounding box at 50% brightness
        if (s == activeStep) {
            NT_drawShapeI(kNT_box,
                          s * slotW, frameY0,
                          s * slotW + slotW - 1, frameY1,
                          8);
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
    const int melWave        = a->v[kParamMelWaveform];
    const int melLoopLength  = a->v[kParamMelLoopLength] > 0 ? a->v[kParamMelLoopLength] : 1;
    const int melRange       = a->v[kParamMelRange];
    const int melOffset      = a->v[kParamMelOffset];
    const int melDensity     = a->v[kParamMelDensity];
    const float melGlideSecs = a->v[kParamMelGlide] * 0.01f;
    const int melGlideCurve  = a->v[kParamMelGlideCurve];
    const int melNoteUpd     = a->v[kParamMelNoteUpdate];
    const float melFoldSg  = a->melFoldSg;
    const float melBpfB0   = a->melBpfB0;
    const float melBpfA1   = a->melBpfA1;
    const float melBpfA2   = a->melBpfA2;
    const float melFeedback = a->melFeedback;
    float melBpfS1   = d->melBpfS1;
    float melBpfS2   = d->melBpfS2;
    float prevBpfOut = d->prevBpfOut;

    // Bass locals from DTC
    float bassPhase          = d->bassPhase;
    float bassFreq           = d->bassFreq;
    float bassTargetFreq     = d->bassTargetFreq;
    float bassGlideStartFreq = d->bassGlideStartFreq;
    float bassGlideProgress  = d->bassGlideProgress;
    float bassGlideDT        = d->bassGlideDT;
    float bassLevelV         = d->bassLevelV;
    float prevClockTrig      = d->prevClockTrig;
    float prevBassSample     = d->prevBassSample;
    const int glideCurve     = a->v[kParamGlideCurve]; // 0=exp, 50=linear, 100=log
    // Bass gate state from SRAM
    bool  bassGated        = a->bassGated;
    bool  bassPendingGated = a->bassPendingGated;
    bool  bassHasPending   = a->bassHasPending;
    int   bassLoopPos      = a->bassLoopPos;
    int   bassNoteUpdateCount    = a->bassNoteUpdateCount;
    bool  bassHasPendingNote     = a->bassHasPendingNote;
    float bassNoteUpdateTargetHz = a->bassNoteUpdateTargetHz;
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
    // Melody gate state from SRAM
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

    // Load DTC into locals for the hot loop
    float prevTrig  = d->prevAdvTrig;
    float prevReset = d->prevResetTrig;
    float ph0 = d->osc_phase[0], ph1 = d->osc_phase[1];
    float ph2 = d->osc_phase[2], ph3 = d->osc_phase[3];
    float fp0 = d->fmPhase[0],   fp1 = d->fmPhase[1];   // modulator phases
    float fp2 = d->fmPhase[2],   fp3 = d->fmPhase[3];
    float f0  = d->osc_freq[0],  f1  = d->osc_freq[1];
    float f2  = d->osc_freq[2],  f3  = d->osc_freq[3];
    float lfoX = d->lfoX, lfoY = d->lfoY;
    const float lfoCos = a->lfoCos, lfoSin = a->lfoSin;
    float lfo2X = d->lfo2X, lfo2Y = d->lfo2Y;
    const float lfo2Cos = a->lfoCos2, lfo2Sin = a->lfoSin2;
    const float gn0 = a->osc_gainF[0], gn1 = a->osc_gainF[1];
    const float gn2 = a->osc_gainF[2], gn3 = a->osc_gainF[3];
    // Pre-split oscillator level into static base and LFO1-modulated portion.
    // elv_i = base_i + mod_i * lfoOut_i,  lfoOut ∈ [-1,+1]
    // halfDepth = depth/2 → max(elv) = osc_levelV (never exceeded at depth=1)
    const float halfDepth = a->levelModDepth * 0.5f;
    const float base0 = d->osc_levelV[0] * (1.0f - halfDepth);
    const float base1 = d->osc_levelV[1] * (1.0f - halfDepth);
    const float base2 = d->osc_levelV[2] * (1.0f - halfDepth);
    const float base3 = d->osc_levelV[3] * (1.0f - halfDepth);
    const float mod0  = d->osc_levelV[0] * halfDepth;
    const float mod1  = d->osc_levelV[1] * halfDepth;
    const float mod2  = d->osc_levelV[2] * halfDepth;
    const float mod3  = d->osc_levelV[3] * halfDepth;
    // Pre-split oscillator shape into static base and LFO2-modulated portion.
    const float shapeHalfDepth = a->shapeModDepth * 0.5f;
    const float sgBase0 = gn0 * (1.0f - shapeHalfDepth);
    const float sgBase1 = gn1 * (1.0f - shapeHalfDepth);
    const float sgBase2 = gn2 * (1.0f - shapeHalfDepth);
    const float sgBase3 = gn3 * (1.0f - shapeHalfDepth);
    const float sgMod0  = gn0 * shapeHalfDepth;
    const float sgMod1  = gn1 * shapeHalfDepth;
    const float sgMod2  = gn2 * shapeHalfDepth;
    const float sgMod3  = gn3 * shapeHalfDepth;
    // Per-oscillator FM: each osc modulated by a sine at its own freq * fmFreqMult.
    // PM formula: output = sinf(2π*(ph+phOff) + fmDepth * sinf(2π*(fmPh+fpOff)))
    const float fmFreqMult = a->fmFreqMult;
    const float fmDepth    = a->fmDepth;
    const bool  fmPost     = (a->v[kParamFMPrePost] != 0);
    // Phase offsets: 8 waves interleaved [Osc1,FM1,Osc2,FM2,Osc3,FM3,Osc4,FM4],
    // each offset by phaseStep (= Phase/360) relative to the previous wave.
    const float ps    = a->phaseStep;
    const float phOff0 = 0.0f;          // Osc1: wave 0
    const float fpOff0 = ps;            // FM1:  wave 1
    const float phOff1 = ps * 2.0f;    // Osc2: wave 2
    const float fpOff1 = ps * 3.0f;    // FM2:  wave 3
    const float phOff2 = ps * 4.0f;    // Osc3: wave 4
    const float fpOff2 = ps * 5.0f;    // FM3:  wave 5
    const float phOff3 = ps * 6.0f;    // Osc4: wave 6
    const float fpOff3 = ps * 7.0f;    // FM4:  wave 7
    float bdt[4] = { f0/sr, f1/sr, f2/sr, f3/sr };

    for (int i = 0; i < numFrames; ++i) {
        // Rising-edge detection on ResetIn — return to step 0 (1V threshold)
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
                    bdt[0] = f0/sr; bdt[1] = f1/sr; bdt[2] = f2/sr; bdt[3] = f3/sr;
                }
            }
            prevTrig = t;
        }

        // LFO1 — level modulation, rotation matrix, exactly norm-preserving.
        // lfoY ≈ sin(θ) → 0°, lfoX ≈ cos(θ) → 90°, -lfoY → 180°, -lfoX → 270°
        const float nx = lfoCos * lfoX - lfoSin * lfoY;
        const float ny = lfoSin * lfoX + lfoCos * lfoY;
        lfoX = nx; lfoY = ny;
        const float lv0 = base0 + mod0 * lfoY;
        const float lv1 = base1 + mod1 * lfoX;
        const float lv2 = base2 - mod2 * lfoY;
        const float lv3 = base3 - mod3 * lfoX;

        // LFO2 — shape modulation, same quadrature pattern
        const float n2x = lfo2Cos * lfo2X - lfo2Sin * lfo2Y;
        const float n2y = lfo2Sin * lfo2X + lfo2Cos * lfo2Y;
        lfo2X = n2x; lfo2Y = n2y;
        const float eg0 = sgBase0 + sgMod0 * lfo2Y;
        const float eg1 = sgBase1 + sgMod1 * lfo2X;
        const float eg2 = sgBase2 - sgMod2 * lfo2Y;
        const float eg3 = sgBase3 - sgMod3 * lfo2X;

        // ---- Bass oscillator ----
        // Clock edge: advance loop, gate check, new note (silent) or NoteUpdate (held).
        if (clockIn) {
            const float clk = clockIn[i];
            if (prevClockTrig < 1.0f && clk >= 1.0f) {
                bassLoopPos = (bassLoopPos + 1) % loopLength;

                if (!bassGated) {
                    // Silent: select new pitch immediately.
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
                    // Held: count ticks; schedule a note update at the next zero crossing.
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

                // Gate check always applies.
                const bool newGate = (a->densityLoop[bassLoopPos] < (float)bassDensity / 100.0f);
                if (newGate != bassGated) {
                    bassPendingGated = newGate;
                    bassHasPending = true;
                }
            }
            prevClockTrig = clk;
        }

        // Glide: advance progress and apply curve to frequency.
        if (bassGlideProgress < 1.0f) {
            bassGlideProgress += bassGlideDT;
            if (bassGlideProgress >= 1.0f) {
                bassFreq = bassTargetFreq;
                bassGlideProgress = 1.0f;
            } else {
                // Curve: 0=exp (fast start), 50=linear, 100=log (slow start)
                const float t  = bassGlideProgress;
                const float tc = glideCurve * 0.01f;
                float t_shaped;
                if (tc <= 0.5f) {
                    const float a_  = tc * 2.0f;           // 0→1 as tc 0→0.5
                    const float tFast = t * (2.0f - t);    // concave: fast start
                    t_shaped = tFast + a_ * (t - tFast);
                } else {
                    const float a_  = (tc - 0.5f) * 2.0f; // 0→1 as tc 0.5→1
                    const float tSlow = t * t;             // convex: slow start
                    t_shaped = t + a_ * (tSlow - t);
                }
                bassFreq = bassGlideStartFreq + (bassTargetFreq - bassGlideStartFreq) * t_shaped;
            }
        }

        // Bass waveform generation
        float bassSample;
        if (bassWave == 0) {
            bassSample = sinf(2.0f * kPi * bassPhase);
        } else {
            // Triangle: 0→1 at 0.25, 1→−1 at 0.75, −1→0 at 1.0
            bassSample = (bassPhase < 0.25f) ? bassPhase * 4.0f
                       : (bassPhase < 0.75f) ? 2.0f - bassPhase * 4.0f
                                             : (bassPhase - 1.0f) * 4.0f;
        }

        // Zero-crossing: apply pending gate state and/or pending note update.
        if ((bassHasPending || bassHasPendingNote) && (prevBassSample * bassSample <= 0.0f)) {
            if (bassHasPending) {
                if (!bassGated && bassPendingGated)
                    bassNoteUpdateCount = 0; // reset counter when a new held note begins
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

        // Bass output (additive)
        bassOut[i] += bassGated ? bassSample * bassLevelV : 0.0f;

        // Advance bass phase
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

        // Melody waveform generation
        float melCoreSample;
        if (melWave == 0) {
            melCoreSample = sinf(2.0f * kPi * melPhase);
        } else {
            melCoreSample = (melPhase < 0.25f) ? melPhase * 4.0f
                          : (melPhase < 0.75f) ? 2.0f - melPhase * 4.0f
                                               : (melPhase - 1.0f) * 4.0f;
        }

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

        // 4-stage wavefolder → biquad BPF → output, with BPF fed back to folder input.
        // State cleared when not gated so each note starts clean.
        float melSample;
        if (melGated) {
            // Chain input: oscillator + 1-sample-delayed BPF feedback
            float w = melCoreSample + prevBpfOut * melFeedback;
            // 4 wavefold stages, each applying per-stage gain before folding
            w = wfold(w * melFoldSg);
            w = wfold(w * melFoldSg);
            w = wfold(w * melFoldSg);
            w = wfold(w * melFoldSg);
            // Biquad BPF — Direct Form II Transposed (b1=0, b2=-b0)
            // y = b0*x + s1 ;  s1 = -a1*y + s2 ;  s2 = -b0*x - a2*y
            melSample  = melBpfB0 * w + melBpfS1;
            melBpfS1   = -melBpfA1 * melSample + melBpfS2;
            melBpfS2   = -melBpfB0 * w - melBpfA2 * melSample;
            prevBpfOut = melSample;
        } else {
            melSample = 0.0f;
            melBpfS1 = melBpfS2 = prevBpfOut = 0.0f; // clear state for clean note start
        }

        // Melody output (additive)
        melOut[i] += melGated ? melSample * melLevelV : 0.0f;

        // Advance melody phase
        melPhase += melFreq / sr;
        if (melPhase >= 1.0f) melPhase -= 1.0f;
        // ---- End melody oscillator ----

        // FM modulator signals (with phase offset)
        const float m0 = fmDepth * sinf(2.0f * kPi * (fp0 + fpOff0));
        const float m1 = fmDepth * sinf(2.0f * kPi * (fp1 + fpOff1));
        const float m2 = fmDepth * sinf(2.0f * kPi * (fp2 + fpOff2));
        const float m3 = fmDepth * sinf(2.0f * kPi * (fp3 + fpOff3));
        // pre:  wfold(sinf(carrier_phase + mod) * gain)  — FM into waveshaper
        // post: wfold(sinf(carrier_phase) * gain + mod)  — carrier+mod summed into waveshaper
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

        // Advance carrier phases
        ph0 += bdt[0]; if (ph0 >= 1.0f) ph0 -= 1.0f;
        ph1 += bdt[1]; if (ph1 >= 1.0f) ph1 -= 1.0f;
        ph2 += bdt[2]; if (ph2 >= 1.0f) ph2 -= 1.0f;
        ph3 += bdt[3]; if (ph3 >= 1.0f) ph3 -= 1.0f;

        // Advance modulator phases at carrier freq * fmFreqMult.
        // fmFreqMult may be negative — use bidirectional wrap.
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
    d->melBpfS1           = melBpfS1;
    d->melBpfS2           = melBpfS2;
    d->prevBpfOut         = prevBpfOut;
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
