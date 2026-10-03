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
    "6/9", "m7b5", "dim7", "9", "maj9", "min9", "min6", "7sus4", "5th",
    NULL
};

static const char* const rootNames[] = {
    "C0",  "C#0", "D0",  "D#0", "E0",  "F0",  "F#0", "G0",  "G#0", "A0",  "A#0", "B0",
    "C1",  "C#1", "D1",  "D#1", "E1",  "F1",  "F#1", "G1",  "G#1", "A1",  "A#1", "B1",
    "C2",  "C#2", "D2",  "D#2", "E2",  "F2",  "F#2", "G2",  "G#2", "A2",  "A#2", "B2",
    "C3",  "C#3", "D3",  "D#3", "E3",  "F3",  "F#3", "G3",  "G#3", "A3",  "A#3", "B3",
    "C4",  "C#4", "D4",  "D#4", "E4",  "F4",  "F#4", "G4",  "G#4", "A4",  "A#4", "B4",
    "C5",
    NULL
};

static const char* const noteLetters[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// Intervals in semitones — 4 voices per chord type (18 types: 0–17)
static const int8_t chordIntervals[18][4] = {
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
    {  0,  0,  7,   7 }, // 17 5th  (two roots + two fifths; detune applies progressively)
};

static const float kC0Hz = 130.8128f / 8.0f;
static const float kPi   = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Wavetable constants  (melody oscillators only; chord section unchanged)
// ---------------------------------------------------------------------------
#define WT_SIZE    2048
#define WT_MASK    (WT_SIZE - 1)
#define WT_MIPS    9
#define WT_NTABLES (2 + 2 * WT_MIPS)   // sine + tri + 9 saw mips + 9 sqr mips

// Mip harmonic limits: index 0 = most harmonics (lowest-freq use) … index 8 = fewest
static const int kWTMipH[WT_MIPS] = { 512, 256, 128, 64, 32, 16, 8, 4, 2 };

// Shared DRAM — filled once in initialise(), pointer used by all instances
static float* g_wt = nullptr;

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
    kParamProgLength,  // 16
    kParamAdvanceIn,   // 17
    kParamResetIn,     // 18
    kParamProgDiv,     // 19
    kParamOSC1Level,   // 20
    kParamOSC1Shape,   // 21
    kParamOSC2Level,   // 22
    kParamOSC2Shape,   // 23
    kParamOSC3Level,   // 24
    kParamOSC3Shape,   // 25
    kParamOSC4Level,   // 26
    kParamOSC4Shape,   // 27
    kParamDetune,      // 28
    kParamOutput,      // 29
    kParamLevelFreq,   // 30
    kParamLevelMod,    // 31
    kParamShapeFreq,   // 32
    kParamShapeMod,    // 33
    kParamFMFreq,      // 34
    kParamFMDepth,     // 35
    kParamPhase,       // 36
    kParamChordAlgo,   // 37 (was FMPrePost — now Mix/FM/PM/AM)
    kParamOctave,      // 38
    kParamBassWaveform,// 39
    kParamBassLevel,   // 40
    kParamBassOutput,  // 41
    kParamClockIn,     // 42
    kParamBassRange,   // 43
    kParamBassOffset,  // 44
    kParamRandomise,   // 45
    kParamLoopLength,  // 46
    kParamGlide,       // 47
    kParamBassDensity, // 48
    kParamGlideCurve,  // 49
    kParamNoteUpdate,  // 50
    // Melody page
    kParamMelOSC1Wave,   // 51
    kParamMelLevel,      // 52
    kParamMelOutput,     // 53
    kParamMelClockIn,    // 54
    kParamMelRange,      // 55
    kParamMelOffset,     // 56
    kParamMelRandomise,  // 57
    kParamMelLoopLength, // 58
    kParamMelGlide,      // 59
    kParamMelDensity,    // 60
    kParamMelGlideCurve, // 61
    kParamMelNoteUpdate, // 62
    kParamMelOctave,     // 63
    // Melody synth
    kParamMelOSC2Wave,    // 64
    kParamMelAlgo,        // 65
    kParamMelIndex,       // 66
    kParamMelOSC2Partial, // 67
    kParamMelOSC2Semi,    // 68
    kParamMelDetune,      // 69  ±50 cents detune on OSC2
    // QuadLFO extensions
    kParamAmpLFOShape,    // 70
    kParamShpLFOShape,    // 71
    kParamResetOnChrd,    // 72
    // Melody LFO
    kParamMelLFOSpeed,    // 73
    kParamMelLFOShape,    // 74
    kParamMelLFOReset,    // 75
    kParamMelLFOAmt,      // 76
    // Generator page
    kParamGenre,          // 77
    kParamGenProgLen,     // 78
    kParamGenerate,       // 79
    kParamGenRoot,        // 80
    kParamVoiceLeading,   // 81
    kParamBassOctave,     // 82
    kParamChordWave,      // 83
    kParamModWave,        // 84  (kept but hidden — modulator always sine)
    kParamHermite,        // 85
    kNumParams            // 86
};

#define ROOT_PARAM(n) \
    { .name="Root " #n,  .min=0, .max=60, .def=36, \
      .unit=kNT_unitEnum, .scaling=0, .enumStrings=rootNames },
#define CHORD_PARAM(n) \
    { .name="Chord " #n, .min=0, .max=17, .def=1, \
      .unit=kNT_unitEnum, .scaling=0, .enumStrings=chordNames },

static const char* const fmPrePostNames[]    = { "pre", "post", NULL };
static const char* const bassWaveformNames[] = { "sine", "tri",  NULL };
static const char* const melWaveNames[]      = { "Sine", "Tri", "Saw", "Square", NULL };
static const char* const melAlgoNames[]      = { "Mix", "FM", "Swarm", "PM", "AM", NULL };
static const char* const chordAlgoNames[]    = { "Mix", "FM", "PM", "AM", NULL };
static const char* const lfoShapeNames[]     = { "sine", "saw", "ramp", "square", NULL };
static const char* const genreNames[]        = { "Pop", "Neo-Soul", "Jazz", "Trance", "Horror", NULL };
static const char* const genRootNames[]      = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", NULL };

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
    { .name="Detune",    .min=0,  .max=33,  .def=0,
      .unit=kNT_unitCents,   .scaling=0,            .enumStrings=NULL },
    NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )
    // QuadLFO: 0.01–20.00 Hz range (value × 0.01 = Hz)
    { .name="LevelFreq", .min=1,   .max=2000, .def=100,
      .unit=kNT_unitHz,      .scaling=kNT_scaling100, .enumStrings=NULL },
    { .name="LevelMod",  .min=0,   .max=100,  .def=0,
      .unit=kNT_unitPercent, .scaling=0,              .enumStrings=NULL },
    { .name="ShapeFreq", .min=1,   .max=2000, .def=100,
      .unit=kNT_unitHz,      .scaling=kNT_scaling100, .enumStrings=NULL },
    { .name="ShapeMod",  .min=0,   .max=100,  .def=0,
      .unit=kNT_unitPercent, .scaling=0,              .enumStrings=NULL },
    { .name="FMFreq",    .min=-4,  .max=16,   .def=1,
      .unit=kNT_unitNone,    .scaling=0,              .enumStrings=NULL },
    { .name="ChordIndex",.min=0,   .max=1000, .def=0,
      .unit=kNT_unitPercent, .scaling=kNT_scaling10,  .enumStrings=NULL },
    { .name="Phase",     .min=0,   .max=3600, .def=0,
      .unit=kNT_unitDegrees, .scaling=kNT_scaling10,  .enumStrings=NULL },
    { .name="ChordAlgo", .min=0,   .max=3,    .def=0,
      .unit=kNT_unitEnum,    .scaling=0,              .enumStrings=chordAlgoNames },
    { .name="Octave",    .min=-2,  .max=2,    .def=0,
      .unit=kNT_unitNone,    .scaling=0,              .enumStrings=NULL },
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
    // Melody page
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
    // Melody synth
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
    { .name="MelDetune",   .min=-50,.max=50,  .def=0,
      .unit=kNT_unitCents,   .scaling=0,               .enumStrings=NULL },
    // QuadLFO extensions
    { .name="AmpLFOShape", .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=lfoShapeNames },
    { .name="ShpLFOShape", .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=lfoShapeNames },
    { .name="ResetOnChrd", .min=0,  .max=1,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // Melody LFO
    { .name="MelLFOSpeed", .min=1,  .max=2000,.def=100,
      .unit=kNT_unitHz,      .scaling=kNT_scaling100,  .enumStrings=NULL },
    { .name="MelLFOShape", .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=lfoShapeNames },
    { .name="MelLFOReset", .min=0,  .max=1,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="MelLFOAmt",  .min=0,  .max=100, .def=0,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
    // Generator page
    { .name="Genre",      .min=0,  .max=4,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=genreNames },
    { .name="ProgLength", .min=1,  .max=8,   .def=4,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="Generate!",  .min=0,  .max=1,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="Root",       .min=0,  .max=11,  .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=genRootNames },
    { .name="VoiceLeading",.min=0, .max=3,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="Octave..",   .min=-4, .max=4,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    { .name="ChordWave", .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=melWaveNames },
    { .name="ModWave",   .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=melWaveNames },
    { .name="Hermite",   .min=0,  .max=1,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
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
    kParamChordAlgo,
    kParamChordWave,
    kParamHermite,
    kParamPhase,
    kParamOutput,
};

static const uint8_t quadLFOPageParams[] = {
    kParamLevelFreq, kParamLevelMod,
    kParamShapeFreq, kParamShapeMod,
    kParamAmpLFOShape,
    kParamShpLFOShape,
    kParamResetOnChrd,
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
    kParamBassOctave,
};

static const uint8_t melodyPageParams[] = {
    kParamMelOSC1Wave,
    kParamMelAlgo,
    kParamMelIndex,
    kParamMelOSC2Partial,
    kParamMelOSC2Semi,
    kParamMelDetune,
    kParamMelLFOSpeed,
    kParamMelLFOShape,
    kParamMelLFOReset,
    kParamMelLFOAmt,
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

static const uint8_t generatorPageParams[] = {
    kParamGenre,
    kParamGenRoot,
    kParamGenProgLen,
    kParamVoiceLeading,
    kParamGenerate,
};

static const _NT_parameterPage pages[] = {
    { .name="Progression", .numParams=ARRAY_SIZE(progressionPageParams), .group=0, .params=progressionPageParams },
    { .name="Oscillators",  .numParams=ARRAY_SIZE(oscillatorsPageParams), .group=0, .params=oscillatorsPageParams },
    { .name="QuadLFO",      .numParams=ARRAY_SIZE(quadLFOPageParams),     .group=0, .params=quadLFOPageParams },
    { .name="Bass",         .numParams=ARRAY_SIZE(bassPageParams),         .group=0, .params=bassPageParams },
    { .name="Melody",       .numParams=ARRAY_SIZE(melodyPageParams),       .group=0, .params=melodyPageParams },
    { .name="Generator",    .numParams=ARRAY_SIZE(generatorPageParams),    .group=0, .params=generatorPageParams },
};

static const _NT_parameterPages paramPages = {
    .numPages = 6,
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
    // QuadLFO — phase accumulators + sine rotation-matrix state
    float lfoPhase1;    // level LFO phase [0,1)
    float lfoPhase2;    // shape LFO phase [0,1)
    float lfo1S, lfo1C; // sin/cos of level LFO phase (for zero-cost sine shape)
    float lfo2S, lfo2C; // sin/cos of shape LFO phase (for zero-cost sine shape)
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
    // Melody oscillator 2 + swarm
    float mel2Phase;
    float melSwarmPhase[7];
    // Melody LFO
    float melLFOPhase;
    // Melody fade envelope (click prevention on gate-on/off)
    float melFadeGain;  // 0.0 = silent, 1.0 = full level
    // Smoothed parameter values (one-pole LP, prevents zipper noise on knob changes)
    float fmDepthSmoothed;
    float melIndexSmoothed;
    float lv0Smoothed, lv1Smoothed, lv2Smoothed, lv3Smoothed;
    float bassLevelSmoothed;
    float melLevelSmoothed;
};

struct ChordsAlg : public _NT_algorithm {
    ChordsAlg_DTC* dtc;
    int currentStep;
    int   advDivCount;
    float levelModDepth;  // LevelMod/100 * 10V — additive depth for level LFO
    float shapeModDepth;  // ShapeMod/100 * 8   — additive depth for shape LFO
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
    int     melNoteUpdateCount;
    bool    melHasPendingNote;
    float   melNoteUpdateTargetHz;
    float   melOctaveShift;
    // Melody synth cached params
    float   melOSC2FreqScale;  // partial * 2^(semi/12 + detune/1200)
    float    melSwarmRatio[7];  // 2^((i+1)*centsPerCopy/1200) for i=0..6
    float    bassOctaveScale;   // 2^BassOctave — multiplied onto bassFreq in step()
    uint32_t rngState;          // per-instance PRNG for chord generation

    ChordsAlg(ChordsAlg_DTC* d) : dtc(d), currentStep(0), advDivCount(0),
                                   levelModDepth(0.0f), shapeModDepth(0.0f),
                                   fmFreqMult(1.0f), fmDepth(0.0f), phaseStep(0.0f),
                                   bassLoopPos(0), bassGated(false),
                                   bassPendingGated(false), bassHasPending(false),
                                   bassNoteUpdateCount(0), bassHasPendingNote(false),
                                   bassNoteUpdateTargetHz(0.0f),
                                   melLoopPos(0), melGated(false),
                                   melPendingGated(false), melHasPending(false),
                                   melNoteUpdateCount(0), melHasPendingNote(false),
                                   melNoteUpdateTargetHz(0.0f),
                                   melOctaveShift(1.0f),
                                   melOSC2FreqScale(1.0f),
                                   bassOctaveScale(1.0f),
                                   rngState(0x12345678u) {
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

static inline float wfold(float x) {
    x += 1.0f;
    float t = x - 4.0f * floorf(x * 0.25f);
    return (t < 2.0f) ? (t - 1.0f) : (3.0f - t);
}

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

// Band-limited oscillator waveform (Saw and Square use PolyBLEP).
static inline float oscWave(int wave, float phase, float dt) {
    switch (wave) {
    default:
    case 0: return sinf(2.0f * kPi * phase);
    case 1: return (phase < 0.25f) ?  phase * 4.0f
                 : (phase < 0.75f) ?  2.0f - phase * 4.0f
                                   : (phase - 1.0f) * 4.0f;
    case 2: { float s = 2.0f * phase - 1.0f; s -= polyblep(phase, dt); return s; }
    case 3: {
        float s = phase < 0.5f ? 1.0f : -1.0f;
        s += polyblep(phase, dt);
        float p2 = phase + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
        s -= polyblep(p2, dt);
        return s;
    }
    }
}

// Positive LFO waveform [0,1]: sine=smooth bell, saw=0→1, ramp=1→0, square=1then0.
// Computes all 4 quadrature values (0°/90°/180°/270°) in one call.
// phase must be in [0,1).
static inline void posWave4(int shape, float phase,
                            float* w0, float* w1, float* w2, float* w3) {
    switch (shape) {
    default:
    case 0: { // Sine: single sinf+cosf, all 4 values from trig identities
        const float s = sinf(2.0f * kPi * phase);
        const float c = cosf(2.0f * kPi * phase);
        *w0 = 0.5f * (1.0f + s);   // sin(θ)
        *w1 = 0.5f * (1.0f + c);   // sin(θ+90°) = cos(θ)
        *w2 = 0.5f * (1.0f - s);   // sin(θ+180°)
        *w3 = 0.5f * (1.0f - c);   // sin(θ+270°)
        break;
    }
    case 1: { // Saw: 0→1 rising
        float p1 = phase + 0.25f; if (p1 >= 1.0f) p1 -= 1.0f;
        float p2 = phase + 0.50f; if (p2 >= 1.0f) p2 -= 1.0f;
        float p3 = phase + 0.75f; if (p3 >= 1.0f) p3 -= 1.0f;
        *w0 = phase; *w1 = p1; *w2 = p2; *w3 = p3;
        break;
    }
    case 2: { // Ramp: 1→0 falling
        float p1 = phase + 0.25f; if (p1 >= 1.0f) p1 -= 1.0f;
        float p2 = phase + 0.50f; if (p2 >= 1.0f) p2 -= 1.0f;
        float p3 = phase + 0.75f; if (p3 >= 1.0f) p3 -= 1.0f;
        *w0 = 1.0f - phase; *w1 = 1.0f - p1; *w2 = 1.0f - p2; *w3 = 1.0f - p3;
        break;
    }
    case 3: { // Square: 1 for first half, 0 for second half
        *w0 = phase < 0.5f ? 1.0f : 0.0f;
        float p1 = phase + 0.25f; if (p1 >= 1.0f) p1 -= 1.0f; *w1 = p1 < 0.5f ? 1.0f : 0.0f;
        float p2 = phase + 0.50f; if (p2 >= 1.0f) p2 -= 1.0f; *w2 = p2 < 0.5f ? 1.0f : 0.0f;
        float p3 = phase + 0.75f; if (p3 >= 1.0f) p3 -= 1.0f; *w3 = p3 < 0.5f ? 1.0f : 0.0f;
        break;
    }
    }
}

// Single positive LFO value (used for melody LFO).
static inline float posWave1(int shape, float phase) {
    switch (shape) {
    default:
    case 0: return 0.5f * (1.0f + sinf(2.0f * kPi * phase));
    case 1: return phase;
    case 2: return 1.0f - phase;
    case 3: return phase < 0.5f ? 1.0f : 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Wavetable oscillator (melody section)
// ---------------------------------------------------------------------------

// Request shared DRAM for wavetable data (called once at plugin load).
void calculateStaticRequirements(_NT_staticRequirements& req) {
    req.dram = (uint32_t)(WT_NTABLES * WT_SIZE * sizeof(float));
}

// Add amp * sin(2pi*n*i/WT_SIZE) to tbl[i=0..WT_SIZE-1] via rotation-matrix
// recurrence — avoids one sinf call per sample during fill.
static inline void wtAddHarmonic(float* tbl, int n, float amp) {
    const float dx   = 2.0f * kPi * (float)n / (float)WT_SIZE;
    const float cosD = cosf(dx);
    const float sinD = sinf(dx);
    float s = 0.0f, c = 1.0f;          // sin(0)=0, cos(0)=1
    for (int i = 0; i < WT_SIZE; ++i) {
        tbl[i] += amp * s;
        const float ns = s * cosD + c * sinD;
        c = c * cosD - s * sinD;
        s = ns;
    }
}

// Fill all wavetables (called once at plugin load, after calculateStaticRequirements).
void initialise(_NT_staticMemoryPtrs& ptrs, const _NT_staticRequirements&) {
    g_wt = (float*)ptrs.dram;

    // --- Sine (no aliasing; single table) ---
    float* tSine = g_wt;
    for (int i = 0; i < WT_SIZE; ++i)
        tSine[i] = sinf(2.0f * kPi * (float)i / (float)WT_SIZE);

    // --- Triangle (full-bandwidth single table; 1/n² rolloff makes aliasing inaudible) ---
    // f(x) = Σ (8/(π²·(2k+1)²))·(-1)^k · sin(2π·(2k+1)·x)
    float* tTri = g_wt + WT_SIZE;
    memset(tTri, 0, WT_SIZE * sizeof(float));
    for (int k = 0; (2*k+1) <= WT_SIZE/2; ++k) {
        const int   n   = 2*k + 1;
        const float amp = (8.0f / (kPi * kPi * (float)(n*n))) * ((k & 1) ? -1.0f : 1.0f);
        wtAddHarmonic(tTri, n, amp);
    }

    // --- Saw mip levels (rising bipolar: −1→+1 over one period) ---
    // f(x) = −(2/π) · Σ_{n=1}^{N} sin(2π·n·x) / n
    for (int m = 0; m < WT_MIPS; ++m) {
        float* tSaw = g_wt + (2 + m) * WT_SIZE;
        memset(tSaw, 0, WT_SIZE * sizeof(float));
        const int N = kWTMipH[m];
        for (int n = 1; n <= N; ++n)
            wtAddHarmonic(tSaw, n, -2.0f / (kPi * (float)n));
    }

    // --- Square mip levels (+1 first half, −1 second half) ---
    // f(x) = (4/π) · Σ_{n=1,3,5,...}^{N} sin(2π·n·x) / n
    for (int m = 0; m < WT_MIPS; ++m) {
        float* tSqr = g_wt + (2 + WT_MIPS + m) * WT_SIZE;
        memset(tSqr, 0, WT_SIZE * sizeof(float));
        const int N = kWTMipH[m];
        for (int n = 1; n <= N; n += 2)
            wtAddHarmonic(tSqr, n, 4.0f / (kPi * (float)n));
    }
}

// Linear-interpolation read from a 2048-point wavetable (phase in [0,1)).
static inline float wtRead(const float* tbl, float phase) {
    const float pos  = phase * (float)WT_SIZE;
    const int   i0   = (int)pos & WT_MASK;
    const float frac = pos - (float)i0;
    return tbl[i0] + frac * (tbl[(i0 + 1) & WT_MASK] - tbl[i0]);
}

// 4-point Hermite (Catmull-Rom) wavetable read.
static inline float wtReadH(const float* tbl, float phase) {
    const float pos = phase * (float)WT_SIZE;
    const int   i1  = (int)pos & WT_MASK;
    const float t   = pos - (float)i1;
    const float p0  = tbl[(i1 - 1) & WT_MASK];
    const float p1  = tbl[i1];
    const float p2  = tbl[(i1 + 1) & WT_MASK];
    const float p3  = tbl[(i1 + 2) & WT_MASK];
    const float c1  = 0.5f * (p2 - p0);
    const float c2  = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    const float c3  = 0.5f * (p3 - p0) + 1.5f * (p1 - p2);
    return ((c3 * t + c2) * t + c1) * t + p1;
}

// Return the best mip index for a given phase increment dt = freq/sr.
// Selects the table with the most harmonics that still avoids aliasing.
static inline int wtMip(float dt) {
    if (dt <= 0.0f) return 0;
    const float maxH = 0.5f / dt;
    for (int m = 0; m < WT_MIPS; ++m)
        if ((float)kWTMipH[m] <= maxH) return m;
    return WT_MIPS - 1;
}

// Wavetable oscillator used for all melody voices (OSC1, OSC2, swarm copies).
// Replaces the PolyBLEP oscWave() in the melody signal path only.
static inline float melOscWave(int wave, float phase, float dt, bool h) {
    if (h) {
        switch (wave) {
        default:
        case 0: return wtReadH(g_wt, phase);
        case 1: return wtReadH(g_wt + WT_SIZE, phase);
        case 2: return wtReadH(g_wt + (2 + wtMip(dt)) * WT_SIZE, phase);
        case 3: return wtReadH(g_wt + (2 + WT_MIPS + wtMip(dt)) * WT_SIZE, phase);
        }
    } else {
        switch (wave) {
        default:
        case 0: return wtRead(g_wt, phase);
        case 1: return wtRead(g_wt + WT_SIZE, phase);
        case 2: return wtRead(g_wt + (2 + wtMip(dt)) * WT_SIZE, phase);
        case 3: return wtRead(g_wt + (2 + WT_MIPS + wtMip(dt)) * WT_SIZE, phase);
        }
    }
}

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
        if (d->bassFreq > 0.0f && a->notePool[0] > 0.0f) {
            float closestHz   = a->notePool[0];
            float closestDist = fabsf(logf(d->bassFreq / a->notePool[0]));
            for (int i = 1; i < 16; ++i) {
                const float dist = fabsf(logf(d->bassFreq / a->notePool[i]));
                if (dist < closestDist) { closestDist = dist; closestHz = a->notePool[i]; }
            }
            d->bassTargetFreq = closestHz;
            const float glideSecs = a->v[kParamGlide] * 0.01f;
            if (glideSecs <= 0.0f || d->bassFreq == closestHz) {
                d->bassFreq         = closestHz;
                d->bassGlideProgress = 1.0f;
                d->bassGlideDT      = 0.0f;
            } else {
                d->bassGlideStartFreq = d->bassFreq;
                d->bassGlideProgress  = 0.0f;
                const float samps = glideSecs * NT_globals.sampleRate;
                d->bassGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
            }
        }
    }

    updateMelNotePool(a);
    {
        if (d->melFreq > 0.0f && a->melNotePool[0] > 0.0f) {
            float closestHz   = a->melNotePool[0];
            float closestDist = fabsf(logf(d->melFreq / a->melNotePool[0]));
            for (int i = 1; i < 16; ++i) {
                const float dist = fabsf(logf(d->melFreq / a->melNotePool[i]));
                if (dist < closestDist) { closestDist = dist; closestHz = a->melNotePool[i]; }
            }
            d->melTargetFreq = closestHz;
            const float glideSecs = a->v[kParamMelGlide] * 0.01f;
            if (glideSecs <= 0.0f || d->melFreq == closestHz) {
                d->melFreq         = closestHz;
                d->melGlideProgress = 1.0f;
                d->melGlideDT      = 0.0f;
            } else {
                d->melGlideStartFreq = d->melFreq;
                d->melGlideProgress  = 0.0f;
                const float samps = glideSecs * NT_globals.sampleRate;
                d->melGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
            }
        }
    }

    // ResetOnChrd: reset both quad LFOs to phase 0 on every chord change
    if (a->v[kParamResetOnChrd]) {
        d->lfoPhase1 = 0.0f; d->lfoPhase2 = 0.0f;
        d->lfo1S = 0.0f; d->lfo1C = 1.0f;
        d->lfo2S = 0.0f; d->lfo2C = 1.0f;
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
    d->lfoPhase1 = 0.0f; d->lfoPhase2 = 0.0f;
    d->lfo1S = 0.0f; d->lfo1C = 1.0f;
    d->lfo2S = 0.0f; d->lfo2C = 1.0f;
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
    d->melLFOPhase        = 0.0f;
    d->melFadeGain        = 0.0f;
    d->fmDepthSmoothed   = 0.0f;
    d->melIndexSmoothed  = 0.0f;
    d->lv0Smoothed = d->lv1Smoothed = d->lv2Smoothed = d->lv3Smoothed = 2.5f;
    d->bassLevelSmoothed = 5.0f;
    d->melLevelSmoothed  = 5.0f;

    ChordsAlg* a = new (ptrs.sram) ChordsAlg(d);
    a->parameters     = parameters;
    a->parameterPages = &paramPages;
    a->levelModDepth = 0.0f;
    a->shapeModDepth = 0.0f;
    for (int i = 0; i < 4; ++i)
        a->osc_gainF[i] = 1.0f + a->v[kParamOSC1Shape + i*2] * 8.0f / 1000.0f;
    // Init default note pools: C3 major
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
    return a;
}

// ---------------------------------------------------------------------------
// Chord generator
// ---------------------------------------------------------------------------

// 9 scales: 0=Major  1=NatMin  2=Dorian  3=Mixolydian  4=Lydian
//           5=HarmMin  6=MelMin  7=DblHarm  8=HungMin
static const int8_t kScaleIntervals[9][7] = {
    { 0, 2, 4, 5, 7, 9, 11 }, // Major
    { 0, 2, 3, 5, 7, 8, 10 }, // Natural Minor
    { 0, 2, 3, 5, 7, 9, 10 }, // Dorian
    { 0, 2, 4, 5, 7, 9, 10 }, // Mixolydian
    { 0, 2, 4, 6, 7, 9, 11 }, // Lydian
    { 0, 2, 3, 5, 7, 8, 11 }, // Harmonic Minor
    { 0, 2, 3, 5, 7, 9, 11 }, // Melodic Minor
    { 0, 1, 4, 5, 7, 8, 11 }, // Double Harmonic
    { 0, 2, 3, 6, 7, 8, 11 }, // Hungarian Minor
};

// Chord types per scale degree: 4=maj7  5=min7  3=dom7  10=m7b5  11=dim7
static const uint8_t kScaleChordTypes[9][7] = {
    {  4,  5,  5,  4,  3,  5, 10 }, // Major
    {  5, 10,  4,  5,  3,  4,  3 }, // Natural Minor  (deg4 = borrowed V7)
    {  5,  5,  4,  3,  5, 10,  4 }, // Dorian
    {  3,  5, 10,  4,  5,  5,  4 }, // Mixolydian
    {  4,  3,  5, 10,  4,  5,  5 }, // Lydian
    {  5, 10,  4,  5,  3,  4, 11 }, // Harmonic Minor
    {  5,  5,  4,  3,  3, 10, 10 }, // Melodic Minor
    {  4,  4,  3,  5,  3,  4, 11 }, // Double Harmonic
    {  5, 10,  4, 11,  3,  4, 11 }, // Hungarian Minor
};

// Harmonic functions: 0=Tonic  1=Subdominant  2=Dominant
static const uint8_t kScaleFunctions[9][7] = {
    { 0, 1, 0, 1, 2, 0, 2 }, // Major
    { 0, 1, 0, 1, 2, 1, 1 }, // Natural Minor
    { 0, 1, 0, 1, 1, 1, 0 }, // Dorian       (no strong D; VII acts as T)
    { 0, 1, 1, 1, 1, 0, 2 }, // Mixolydian
    { 0, 1, 0, 1, 0, 1, 1 }, // Lydian       (no D — II7 fills SD role)
    { 0, 1, 0, 1, 2, 1, 2 }, // Harmonic Minor
    { 0, 1, 0, 1, 2, 1, 2 }, // Melodic Minor
    { 0, 2, 1, 1, 2, 1, 2 }, // Double Harmonic
    { 0, 1, 0, 2, 2, 1, 2 }, // Hungarian Minor
};

// Genre → scale options (4 per genre)
static const uint8_t kGenreScales[5][4] = {
    { 0, 1, 2, 3 }, // Pop:      Major, NatMin, Dorian, Mixolydian
    { 2, 0, 1, 6 }, // Neo-soul: Dorian, Major, NatMin, MelMin
    { 6, 0, 5, 2 }, // Jazz:     MelMin, Major, HarmMin, Dorian
    { 1, 0, 5, 3 }, // Trance:   NatMin, Major, HarmMin, Mixolydian
    { 7, 8, 5, 5 }, // Horror:   DblHarm, HungMin, HarmMin, HarmMin
};

// Progression patterns (8 slots; slice from index [8-length] for desired length)
static const uint8_t kGenPatterns[5][8] = {
    { 1, 0, 1, 2, 0, 1, 2, 0 }, // Pop      ends T
    { 0, 1, 1, 2, 0, 1, 2, 0 }, // Neo-soul ends T
    { 1, 0, 1, 2, 0, 1, 2, 0 }, // Jazz     ends T
    { 2, 0, 1, 2, 0, 1, 2, 0 }, // Trance   ends T
    { 2, 1, 2, 2, 1, 2, 2, 2 }, // Horror   avoids T resolution
};

// 7th → triad reduction (18 entries, index = chord type)
// dom7(3)→maj(1), maj7(4)→maj(1), min7(5)→min(2), m7b5(10)→min(2), dim7(11)→min(2)
static const uint8_t kTo3Note[18] = {
    0, 1, 2, 1, 1, 2, 6, 7, 8, 1, 2, 2, 1, 1, 2, 2, 7, 17
};

static inline uint32_t genLcgNext(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s;
}
static inline int genLcgRange(uint32_t& s, int n) {
    if (n <= 1) return 0;
    return (int)((genLcgNext(s) >> 16) % (uint32_t)n);
}

// 12-bit bitmask of note classes present in a chord (mod 12, all 4 voices)
static uint16_t chordNoteSet(int rootNoteClass, int chordType) {
    uint16_t mask = 0;
    for (int n = 0; n < 4; ++n) {
        int nc = ((rootNoteClass + (int)chordIntervals[chordType][n]) % 12 + 12) % 12;
        mask |= (uint16_t)(1u << nc);
    }
    return mask;
}
static int countBits(uint16_t x) {
    int c = 0;
    while (x) { c += x & 1; x >>= 1; }
    return c;
}

static void generateProgression(ChordsAlg* a) {
    const int genre    = a->v[kParamGenre];
    const int len      = a->v[kParamGenProgLen];
    const int rootNote = a->v[kParamGenRoot];
    const int vlTarget = a->v[kParamVoiceLeading];
    if (len < 1 || len > 8) return;

    const int scaleIdx = kGenreScales[genre][genLcgRange(a->rngState, 4)];

    // Sort degrees into harmonic-function buckets
    int tDeg[7], sdDeg[7], dDeg[7];
    int nT = 0, nSD = 0, nD = 0;
    for (int i = 0; i < 7; ++i) {
        switch (kScaleFunctions[scaleIdx][i]) {
            case 0: tDeg[nT++]   = i; break;
            case 1: sdDeg[nSD++] = i; break;
            default: dDeg[nD++]  = i; break;
        }
    }

    const uint8_t* pat  = kGenPatterns[genre];
    const int32_t algIdx = NT_algorithmIndex(a);
    const int     offset = (int)NT_parameterOffset();

    int      firstDeg    = -1;
    uint16_t prevNoteSet = 0;

    for (int pos = 0; pos < len; ++pos) {
        const int func = pat[8 - len + pos];

        // Build shuffled candidate list: primary function bucket first, rest after
        int cands[7]; int nCands = 0;
        {
            int* prim; int nPrim;
            if      (func == 0 && nT  > 0) { prim = tDeg;  nPrim = nT;  }
            else if (func == 1 && nSD > 0) { prim = sdDeg; nPrim = nSD; }
            else if (func == 2 && nD  > 0) { prim = dDeg;  nPrim = nD;  }
            else if (nT  > 0)              { prim = tDeg;  nPrim = nT;  }
            else if (nSD > 0)              { prim = sdDeg; nPrim = nSD; }
            else                           { prim = dDeg;  nPrim = nD;  }
            for (int i = 0; i < nPrim; ++i) cands[nCands++] = prim[i];
            // Append remaining degrees (other function buckets)
            for (int d = 0; d < 7; ++d) {
                bool found = false;
                for (int j = 0; j < nCands; ++j) if (cands[j] == d) { found = true; break; }
                if (!found) cands[nCands++] = d;
            }
            // Shuffle just the primary portion
            for (int i = nPrim - 1; i > 0; --i) {
                int j = genLcgRange(a->rngState, i + 1);
                int tmp = cands[i]; cands[i] = cands[j]; cands[j] = tmp;
            }
        }

        // Chord complexity: genre-dependent
        int complexity;
        if (genre == 1 || genre == 2) {
            // Neo-soul, Jazz: always 4-note (rich voicings)
            complexity = 4;
        } else if (genre == 3) {
            // Trance: 3-note or 2-note only (open, clear)
            complexity = (genLcgRange(a->rngState, 2) == 0) ? 3 : 2;
        } else {
            // Pop, Horror: mixed 4/3/2 (60/30/10)
            const int roll = genLcgRange(a->rngState, 10);
            complexity = (roll <= 5) ? 4 : (roll <= 8) ? 3 : 2;
        }

        // Try to find a candidate satisfying voice-leading and first/last constraints
        int chosenDeg = -1, chosenType = -1;
        // Pass 1: respect VL exactly
        // Pass 2: ignore VL (fallback)
        for (int pass = 0; pass < 2 && chosenDeg < 0; ++pass) {
            for (int ci = 0; ci < nCands; ++ci) {
                const int deg = cands[ci];
                // Last chord must differ from first chord
                if (pos == len - 1 && deg == firstDeg && nCands > 1) continue;
                const int base = kScaleChordTypes[scaleIdx][deg];
                const int type = (complexity == 4) ? base :
                                 (complexity == 3) ? (int)kTo3Note[base] : 17;
                const int nc   = (rootNote + (int)kScaleIntervals[scaleIdx][deg]) % 12;
                const uint16_t ns = chordNoteSet(nc, type);
                if (pass == 1 || pos == 0 || countBits(prevNoteSet & ns) == vlTarget) {
                    chosenDeg = deg; chosenType = type; break;
                }
            }
        }
        // Final fallback (shouldn't be needed)
        if (chosenDeg < 0) { chosenDeg = cands[0]; chosenType = kScaleChordTypes[scaleIdx][cands[0]]; }

        if (pos == 0) firstDeg = chosenDeg;
        const int noteClass = (rootNote + (int)kScaleIntervals[scaleIdx][chosenDeg]) % 12;
        prevNoteSet = chordNoteSet(noteClass, chosenType);

        NT_setParameterFromAudio(algIdx, kParamRoot1 + pos * 2     + offset, (int16_t)(36 + noteClass));
        NT_setParameterFromAudio(algIdx, kParamRoot1 + pos * 2 + 1 + offset, (int16_t)chosenType);
    }

    NT_setParameterFromAudio(algIdx, kParamProgLength + offset, (int16_t)len);
    NT_setParameterFromAudio(algIdx, kParamGenerate   + offset, 0);
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
    } else if (p == kParamLevelMod) {
        // LFO adds to level; depth in same units as osc_levelV (volts, 0–10V)
        a->levelModDepth = a->v[kParamLevelMod] / 100.0f * 10.0f;
    } else if (p == kParamShapeMod) {
        // LFO adds to gain; gain range is 1.0–9.0 (span 8)
        a->shapeModDepth = a->v[kParamShapeMod] / 100.0f * 8.0f;
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
        if (a->v[kParamRandomise] == 1) {
            for (int i = 0; i < 16; ++i) {
                a->melodyLoop[i]  = (float)(lcg_next() & 0xFFFFu) / 65535.0f;
                a->densityLoop[i] = (float)(lcg_next() & 0xFFFFu) / 65535.0f;
            }
            NT_setParameterFromAudio(NT_algorithmIndex(a), kParamRandomise + NT_parameterOffset(), 0);
        }
    } else if (p == kParamMelLevel) {
        a->dtc->melLevelV = a->v[kParamMelLevel] / 100.0f * 10.0f;
    } else if (p == kParamMelRandomise) {
        if (a->v[kParamMelRandomise] == 1) {
            for (int i = 0; i < 16; ++i) {
                a->melMelodyLoop[i]  = (float)(lcg_next() & 0xFFFFu) / 65535.0f;
                a->melDensityLoop[i] = (float)(lcg_next() & 0xFFFFu) / 65535.0f;
            }
            NT_setParameterFromAudio(NT_algorithmIndex(a), kParamMelRandomise + NT_parameterOffset(), 0);
        }
    } else if (p == kParamMelOctave) {
        updateFrequencies(a);
    } else if (p == kParamMelOSC2Partial || p == kParamMelOSC2Semi || p == kParamMelDetune) {
        const int partial = a->v[kParamMelOSC2Partial];
        const int semi    = a->v[kParamMelOSC2Semi];
        const int detune  = a->v[kParamMelDetune]; // -50..+50 cents
        a->melOSC2FreqScale = (float)partial *
            powf(2.0f, (float)semi / 12.0f + (float)detune / 1200.0f);
    } else if (p == kParamMelIndex) {
        const float centsPerCopy = (float)a->v[kParamMelIndex] / 1000.0f * 50.0f;
        for (int i = 0; i < 7; ++i)
            a->melSwarmRatio[i] = powf(2.0f, (float)(i + 1) * centsPerCopy / 1200.0f);
    } else if (p == kParamBassOctave) {
        a->bassOctaveScale = powf(2.0f, (float)a->v[kParamBassOctave]);
    } else if (p == kParamGenerate) {
        if (a->v[kParamGenerate] == 1)
            generateProgression(a);
    }
    // LevelFreq, ShapeFreq, AmpLFOShape, ShpLFOShape, ResetOnChrd,
    // MelLFOSpeed, MelLFOShape, MelLFOReset, MelLFOAmt — all read live from v[] in step().
    // Genre, GenProgLen, GenRoot, Generate! — handled above or read at generate time.
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

        if (s == activeStep)
            NT_drawShapeI(kNT_box, s * slotW, frameY0, s * slotW + slotW - 1, frameY1, 8);

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
                if ((pA&&!cA)||(!pA&&cA)||((p>='a'&&p<='z')&&(c>='A'&&c<='Z')))
                    splitPos = k;
            }
            for (int k = 1; k < totalLen && splitPos < 0; ++k) {
                const char p = combined[k-1], c = combined[k];
                const bool pA = (p>='a'&&p<='z')||(p>='A'&&p<='Z');
                const bool cA = (c>='a'&&c<='z')||(c>='A'&&c<='Z');
                if ((pA&&!cA)||(!pA&&cA)||((p>='a'&&p<='z')&&(c>='A'&&c<='Z')))
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

    float* out     = busFrames + (a->v[kParamOutput]     - 1) * numFrames;
    float* bassOut = busFrames + (a->v[kParamBassOutput] - 1) * numFrames;
    float* melOut  = busFrames + (a->v[kParamMelOutput]  - 1) * numFrames;

    // Bass clock
    const int clockBus   = a->v[kParamClockIn];
    const float* clockIn = (clockBus > 0) ? busFrames + (clockBus - 1) * numFrames : nullptr;
    const int loopLength  = a->v[kParamLoopLength] > 0 ? a->v[kParamLoopLength] : 1;
    const int bassRange   = a->v[kParamBassRange];
    const int bassOffset  = a->v[kParamBassOffset];
    const int bassDensity = a->v[kParamBassDensity];
    const float glideSecs = a->v[kParamGlide] * 0.01f;
    const int glideCurve  = a->v[kParamGlideCurve];
    const int bassWave    = a->v[kParamBassWaveform];
    const int noteUpdate  = a->v[kParamNoteUpdate];

    // Melody clock
    const int melClockBus    = a->v[kParamMelClockIn];
    const float* melClockIn  = (melClockBus > 0) ? busFrames + (melClockBus - 1) * numFrames : nullptr;
    const int melLoopLength  = a->v[kParamMelLoopLength] > 0 ? a->v[kParamMelLoopLength] : 1;
    const int melRange       = a->v[kParamMelRange];
    const int melOffset      = a->v[kParamMelOffset];
    const int melDensity     = a->v[kParamMelDensity];
    const float melGlideSecs = a->v[kParamMelGlide] * 0.01f;
    const int melGlideCurve  = a->v[kParamMelGlideCurve];
    const int melNoteUpd     = a->v[kParamMelNoteUpdate];

    // Melody synth
    const int   melOSC1Wave  = a->v[kParamMelOSC1Wave];
    const int   melAlgo      = a->v[kParamMelAlgo];
    const float melOSC2Scale = a->melOSC2FreqScale;
    float swRat[7];
    for (int i = 0; i < 7; ++i) swRat[i] = a->melSwarmRatio[i];

    // Melody LFO
    const float melLFODt    = (float)a->v[kParamMelLFOSpeed] * 0.01f / sr;
    const int   melLFOShape  = a->v[kParamMelLFOShape];
    const bool  melLFOReset  = (a->v[kParamMelLFOReset] != 0);
    const float melLFOAmt    = (float)a->v[kParamMelLFOAmt] * 0.01f;

    // QuadLFO
    const float lfoDt1       = (float)a->v[kParamLevelFreq] * 0.01f / sr;
    const float lfoDt2       = (float)a->v[kParamShapeFreq] * 0.01f / sr;
    const int   ampLFOShape  = a->v[kParamAmpLFOShape];
    const int   shpLFOShape  = a->v[kParamShpLFOShape];
    const bool  resetOnChrd  = (a->v[kParamResetOnChrd] != 0);
    const float levelModDepth = a->levelModDepth;
    const float shapeModDepth = a->shapeModDepth;
    const float gn0 = a->osc_gainF[0], gn1 = a->osc_gainF[1];
    const float gn2 = a->osc_gainF[2], gn3 = a->osc_gainF[3];

    // Chord oscillator locals
    float prevTrig  = d->prevAdvTrig;
    float prevReset = d->prevResetTrig;
    float ph0 = d->osc_phase[0], ph1 = d->osc_phase[1];
    float ph2 = d->osc_phase[2], ph3 = d->osc_phase[3];
    float fp0 = d->fmPhase[0],   fp1 = d->fmPhase[1];
    float fp2 = d->fmPhase[2],   fp3 = d->fmPhase[3];
    float f0  = d->osc_freq[0],  f1  = d->osc_freq[1];
    float f2  = d->osc_freq[2],  f3  = d->osc_freq[3];
    float bdt[4] = { f0/sr, f1/sr, f2/sr, f3/sr };
    const float fmFreqMult = a->fmFreqMult;
    const int   chordAlgo  = a->v[kParamChordAlgo];
    const int   chordWave  = a->v[kParamChordWave];
    const bool  hermite    = (a->v[kParamHermite] != 0);
    // Parameter smoothing — one-pole LP (~5 ms time constant)
    const float smoothCoeff     = 1.0f - expf(-1.0f / (0.005f * sr));
    float fmDepthSmoothed       = d->fmDepthSmoothed;
    float melIndexSmoothed      = d->melIndexSmoothed;
    float lv0Sm = d->lv0Smoothed, lv1Sm = d->lv1Smoothed;
    float lv2Sm = d->lv2Smoothed, lv3Sm = d->lv3Smoothed;
    float bassLvSm              = d->bassLevelSmoothed;
    float melLvSm               = d->melLevelSmoothed;
    const float lvTgt0 = d->osc_levelV[0], lvTgt1 = d->osc_levelV[1];
    const float lvTgt2 = d->osc_levelV[2], lvTgt3 = d->osc_levelV[3];
    const float bassTgt = d->bassLevelV;
    const float melTgt  = d->melLevelV;
    const float melIndexTarget  = (float)a->v[kParamMelIndex] / 1000.0f;
    const float ps     = a->phaseStep;
    const float phOff0 = 0.0f,      fpOff0 = ps;
    const float phOff1 = ps * 2.0f, fpOff1 = ps * 3.0f;
    const float phOff2 = ps * 4.0f, fpOff2 = ps * 5.0f;
    const float phOff3 = ps * 6.0f, fpOff3 = ps * 7.0f;

    // QuadLFO phase + rotation-matrix locals
    float lfoPhase1 = d->lfoPhase1;
    float lfoPhase2 = d->lfoPhase2;
    float lfo1S = d->lfo1S, lfo1C = d->lfo1C;
    float lfo2S = d->lfo2S, lfo2C = d->lfo2C;
    // Rotation-matrix increment coefficients (2 sinf/cosf per buffer, not per sample)
    const float lfoCosD1 = cosf(2.0f * kPi * lfoDt1);
    const float lfoSinD1 = sinf(2.0f * kPi * lfoDt1);
    const float lfoCosD2 = cosf(2.0f * kPi * lfoDt2);
    const float lfoSinD2 = sinf(2.0f * kPi * lfoDt2);

    // Bass DTC locals
    float bassPhase          = d->bassPhase;
    float bassFreq           = d->bassFreq;
    float bassTargetFreq     = d->bassTargetFreq;
    float bassGlideStartFreq = d->bassGlideStartFreq;
    float bassGlideProgress  = d->bassGlideProgress;
    float bassGlideDT        = d->bassGlideDT;
    float prevClockTrig      = d->prevClockTrig;
    float prevBassSample     = d->prevBassSample;
    bool  bassGated          = a->bassGated;
    bool  bassPendingGated   = a->bassPendingGated;
    bool  bassHasPending     = a->bassHasPending;
    int   bassLoopPos        = a->bassLoopPos;
    int   bassNoteUpdateCount    = a->bassNoteUpdateCount;
    bool  bassHasPendingNote     = a->bassHasPendingNote;
    float bassNoteUpdateTargetHz = a->bassNoteUpdateTargetHz;

    // Melody DTC locals
    float melPhase           = d->melPhase;
    float melFreq            = d->melFreq;
    float melTargetFreq      = d->melTargetFreq;
    float melGlideStartFreq  = d->melGlideStartFreq;
    float melGlideProgress   = d->melGlideProgress;
    float melGlideDT         = d->melGlideDT;
    float prevMelClockTrig   = d->prevMelClockTrig;
    float prevMelSample      = d->prevMelSample;
    float mel2Phase          = d->mel2Phase;
    float swPh[7];
    for (int i = 0; i < 7; ++i) swPh[i] = d->melSwarmPhase[i];
    float melLFOPhase        = d->melLFOPhase;
    float melFadeGain        = d->melFadeGain;
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

    for (int i = 0; i < numFrames; ++i) {
        // ResetIn rising edge
        if (rst) {
            const float r = rst[i];
            if (prevReset < 1.0f && r >= 1.0f) {
                a->currentStep = 0;
                updateFrequencies(a);
                f0 = d->osc_freq[0]; f1 = d->osc_freq[1];
                f2 = d->osc_freq[2]; f3 = d->osc_freq[3];
                bdt[0]=f0/sr; bdt[1]=f1/sr; bdt[2]=f2/sr; bdt[3]=f3/sr;
                if (resetOnChrd) { lfoPhase1 = d->lfoPhase1; lfoPhase2 = d->lfoPhase2; }
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
                    bdt[0]=f0/sr; bdt[1]=f1/sr; bdt[2]=f2/sr; bdt[3]=f3/sr;
                    if (resetOnChrd) { lfoPhase1 = d->lfoPhase1; lfoPhase2 = d->lfoPhase2;
                                       lfo1S = d->lfo1S; lfo1C = d->lfo1C;
                                       lfo2S = d->lfo2S; lfo2C = d->lfo2C; }
                }
            }
            prevTrig = t;
        }

        // Per-sample parameter smoothing
        fmDepthSmoothed  += (a->fmDepth        - fmDepthSmoothed)  * smoothCoeff;
        melIndexSmoothed += (melIndexTarget     - melIndexSmoothed) * smoothCoeff;
        lv0Sm += (lvTgt0 - lv0Sm) * smoothCoeff;
        lv1Sm += (lvTgt1 - lv1Sm) * smoothCoeff;
        lv2Sm += (lvTgt2 - lv2Sm) * smoothCoeff;
        lv3Sm += (lvTgt3 - lv3Sm) * smoothCoeff;
        bassLvSm += (bassTgt - bassLvSm) * smoothCoeff;
        melLvSm  += (melTgt  - melLvSm)  * smoothCoeff;

        // QuadLFO — level modulation (positive, additive)
        // Sine shape: rotation matrix — zero transcendentals per sample.
        // Other shapes: posWave4 arithmetic — also cheap.
        float lw0, lw1, lw2, lw3;
        if (ampLFOShape == 0) {
            lw0 = 0.5f * (1.0f + lfo1S); lw1 = 0.5f * (1.0f + lfo1C);
            lw2 = 0.5f * (1.0f - lfo1S); lw3 = 0.5f * (1.0f - lfo1C);
        } else {
            posWave4(ampLFOShape, lfoPhase1, &lw0, &lw1, &lw2, &lw3);
        }
        { const float ns = lfo1S*lfoCosD1 + lfo1C*lfoSinD1;
          lfo1C = lfo1C*lfoCosD1 - lfo1S*lfoSinD1; lfo1S = ns; }
        lfoPhase1 += lfoDt1; if (lfoPhase1 >= 1.0f) lfoPhase1 -= 1.0f;
        const float lv0 = lv0Sm + levelModDepth * lw0;
        const float lv1 = lv1Sm + levelModDepth * lw1;
        const float lv2 = lv2Sm + levelModDepth * lw2;
        const float lv3 = lv3Sm + levelModDepth * lw3;

        // QuadLFO — shape modulation (positive, additive)
        float sg0, sg1, sg2, sg3;
        if (shpLFOShape == 0) {
            sg0 = 0.5f * (1.0f + lfo2S); sg1 = 0.5f * (1.0f + lfo2C);
            sg2 = 0.5f * (1.0f - lfo2S); sg3 = 0.5f * (1.0f - lfo2C);
        } else {
            posWave4(shpLFOShape, lfoPhase2, &sg0, &sg1, &sg2, &sg3);
        }
        { const float ns = lfo2S*lfoCosD2 + lfo2C*lfoSinD2;
          lfo2C = lfo2C*lfoCosD2 - lfo2S*lfoSinD2; lfo2S = ns; }
        lfoPhase2 += lfoDt2; if (lfoPhase2 >= 1.0f) lfoPhase2 -= 1.0f;
        const float eg0 = gn0 + shapeModDepth * sg0;
        const float eg1 = gn1 + shapeModDepth * sg1;
        const float eg2 = gn2 + shapeModDepth * sg2;
        const float eg3 = gn3 + shapeModDepth * sg3;

        // ---- Bass oscillator ----
        if (clockIn) {
            const float clk = clockIn[i];
            if (prevClockTrig < 1.0f && clk >= 1.0f) {
                bassLoopPos = (bassLoopPos + 1) % loopLength;
                if (!bassGated) {
                    const float mval = a->melodyLoop[bassLoopPos];
                    int ni = bassOffset + (int)(mval * (float)(bassRange + 1));
                    if (ni > 15) ni = 15; if (ni < 0) ni = 0;
                    bassTargetFreq = a->notePool[ni];
                    if (glideSecs <= 0.0f) {
                        bassFreq = bassTargetFreq; bassGlideProgress = 1.0f; bassGlideDT = 0.0f;
                    } else {
                        bassGlideStartFreq = bassFreq; bassGlideProgress = 0.0f;
                        const float samps = glideSecs * sr;
                        bassGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
                    }
                } else if (noteUpdate > 0) {
                    if (++bassNoteUpdateCount >= noteUpdate) {
                        bassNoteUpdateCount = 0;
                        const float mval = a->melodyLoop[bassLoopPos];
                        int ni = bassOffset + (int)(mval * (float)(bassRange + 1));
                        if (ni > 15) ni = 15; if (ni < 0) ni = 0;
                        bassNoteUpdateTargetHz = a->notePool[ni];
                        bassHasPendingNote = true;
                    }
                }
                const bool newGate = (a->densityLoop[bassLoopPos] < (float)bassDensity / 100.0f);
                if (newGate != bassGated) { bassPendingGated = newGate; bassHasPending = true; }
            }
            prevClockTrig = clk;
        }

        if (bassGlideProgress < 1.0f) {
            bassGlideProgress += bassGlideDT;
            if (bassGlideProgress >= 1.0f) {
                bassFreq = bassTargetFreq; bassGlideProgress = 1.0f;
            } else {
                const float t = bassGlideProgress, tc = glideCurve * 0.01f;
                float ts;
                if (tc <= 0.5f) { const float a_ = tc*2.0f, tF = t*(2.0f-t); ts = tF + a_*(t-tF); }
                else             { const float a_ = (tc-0.5f)*2.0f, tS = t*t;  ts = t  + a_*(tS-t); }
                bassFreq = bassGlideStartFreq + (bassTargetFreq - bassGlideStartFreq) * ts;
            }
        }

        const float bassDt = bassFreq / sr;
        float bassSample = melOscWave(bassWave, bassPhase, bassDt, hermite);

        if ((bassHasPending || bassHasPendingNote) && prevBassSample * bassSample <= 0.0f) {
            if (bassHasPending) {
                if (!bassGated && bassPendingGated) bassNoteUpdateCount = 0;
                bassGated = bassPendingGated; bassHasPending = false;
            }
            if (bassHasPendingNote) {
                if (glideSecs <= 0.0f || bassFreq == bassNoteUpdateTargetHz) {
                    bassFreq = bassNoteUpdateTargetHz; bassTargetFreq = bassNoteUpdateTargetHz;
                    bassGlideProgress = 1.0f; bassGlideDT = 0.0f;
                } else {
                    bassTargetFreq = bassNoteUpdateTargetHz; bassGlideStartFreq = bassFreq;
                    bassGlideProgress = 0.0f;
                    const float samps = glideSecs * sr;
                    bassGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
                }
                bassHasPendingNote = false;
            }
        }
        prevBassSample = bassSample;
        bassOut[i] += bassGated ? bassSample * bassLvSm : 0.0f;
        bassPhase += bassFreq * a->bassOctaveScale / sr;
        if (bassPhase >= 1.0f) bassPhase -= 1.0f;
        // ---- End bass ----

        // ---- Melody oscillator ----
        if (melClockIn) {
            const float clk = melClockIn[i];
            if (prevMelClockTrig < 1.0f && clk >= 1.0f) {
                melLoopPos = (melLoopPos + 1) % melLoopLength;
                if (!melGated) {
                    const float mval = a->melMelodyLoop[melLoopPos];
                    int ni = melOffset + (int)(mval * (float)(melRange + 1));
                    if (ni > 15) ni = 15; if (ni < 0) ni = 0;
                    melTargetFreq = a->melNotePool[ni];
                    if (melGlideSecs <= 0.0f) {
                        melFreq = melTargetFreq; melGlideProgress = 1.0f; melGlideDT = 0.0f;
                    } else {
                        melGlideStartFreq = melFreq; melGlideProgress = 0.0f;
                        const float samps = melGlideSecs * sr;
                        melGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
                    }
                } else if (melNoteUpd > 0) {
                    if (++melNoteUpdateCount >= melNoteUpd) {
                        melNoteUpdateCount = 0;
                        const float mval = a->melMelodyLoop[melLoopPos];
                        int ni = melOffset + (int)(mval * (float)(melRange + 1));
                        if (ni > 15) ni = 15; if (ni < 0) ni = 0;
                        melNoteUpdateTargetHz = a->melNotePool[ni];
                        melHasPendingNote = true;
                    }
                }
                const bool newGate = (a->melDensityLoop[melLoopPos] < (float)melDensity / 100.0f);
                if (newGate != melGated) { melPendingGated = newGate; melHasPending = true; }
            }
            prevMelClockTrig = clk;
        }

        if (melGlideProgress < 1.0f) {
            melGlideProgress += melGlideDT;
            if (melGlideProgress >= 1.0f) {
                melFreq = melTargetFreq; melGlideProgress = 1.0f;
            } else {
                const float t = melGlideProgress, tc = melGlideCurve * 0.01f;
                float ts;
                if (tc <= 0.5f) { const float a_ = tc*2.0f, tF = t*(2.0f-t); ts = tF + a_*(t-tF); }
                else             { const float a_ = (tc-0.5f)*2.0f, tS = t*t;  ts = t  + a_*(tS-t); }
                melFreq = melGlideStartFreq + (melTargetFreq - melGlideStartFreq) * ts;
            }
        }

        const float melDt = melFreq / sr;
        float melCoreSample = melOscWave(melOSC1Wave, melPhase, melDt, hermite);

        // Gate changes: apply immediately — fade envelope removes clicks for all algos.
        if (melHasPending) {
            const bool wasGated = melGated;
            if (!melGated && melPendingGated) melNoteUpdateCount = 0;
            melGated = melPendingGated;
            melHasPending = false;
            if (!wasGated && melGated && melLFOReset)
                melLFOPhase = 0.0f;
        }
        // Note-frequency updates still wait for OSC1 zero crossing (no pitch-change click).
        if (melHasPendingNote && prevMelSample * melCoreSample <= 0.0f) {
            if (melGlideSecs <= 0.0f || melFreq == melNoteUpdateTargetHz) {
                melFreq = melNoteUpdateTargetHz; melTargetFreq = melNoteUpdateTargetHz;
                melGlideProgress = 1.0f; melGlideDT = 0.0f;
            } else {
                melTargetFreq = melNoteUpdateTargetHz; melGlideStartFreq = melFreq;
                melGlideProgress = 0.0f;
                const float samps = melGlideSecs * sr;
                melGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
            }
            melHasPendingNote = false;
        }
        prevMelSample = melCoreSample;

        // Fade envelope: ~1.3 ms ramp (64 samples at 48 kHz) on every gate-on/off.
        // Eliminates clicks from all multi-oscillator algos (Swarm, Mix, PM, FM).
        if (melGated) {
            melFadeGain += (1.0f / 64.0f); if (melFadeGain > 1.0f) melFadeGain = 1.0f;
        } else {
            melFadeGain -= (1.0f / 64.0f); if (melFadeGain < 0.0f) melFadeGain = 0.0f;
        }

        // Melody LFO → modulate Index (positive, additive; clamp to [0,1])
        const float melLFOWave = posWave1(melLFOShape, melLFOPhase);
        float effectiveIndexF = melIndexSmoothed + melLFOAmt * melLFOWave;
        if (effectiveIndexF > 1.0f) effectiveIndexF = 1.0f;
        melLFOPhase += melLFODt;
        if (melLFOPhase >= 1.0f) melLFOPhase -= 1.0f;

        // Algo-based synthesis — runs while fade is non-zero (includes fade-out tail)
        float melSample;
        float melPhaseAdv = melDt;

        if (melFadeGain > 0.0f) {
            if (melAlgo == 2) {
                // Swarm: OSC1 + 7 detuned copies, normalised to 1/8
                float sum = melCoreSample;
                for (int ci = 0; ci < 7; ++ci) {
                    const float swDt = melFreq * swRat[ci] / sr;
                    sum += melOscWave(melOSC1Wave, swPh[ci], swDt, hermite);
                    swPh[ci] += swDt;
                    swPh[ci] -= floorf(swPh[ci]);
                }
                melSample = sum * (1.0f / 8.0f);
            } else {
                const float dt2  = melFreq * melOSC2Scale / sr;
                const float osc2 = (dt2 > 0.0f) ? (hermite ? wtReadH(g_wt, mel2Phase) : wtRead(g_wt, mel2Phase)) : 0.0f;
                mel2Phase += dt2;
                mel2Phase -= floorf(mel2Phase);

                switch (melAlgo) {
                case 0: { // Mix — equal-power crossfade
                    const float g1 = cosf(kPi * 0.5f * effectiveIndexF);
                    const float g2 = sinf(kPi * 0.5f * effectiveIndexF);
                    melSample = melCoreSample * g1 + osc2 * g2;
                    break;
                }
                case 1: { // FM — linear FM via modulated phase advance
                    const float fmDt = melDt * (1.0f + osc2 * effectiveIndexF * 10.0f);
                    melSample = melOscWave(melOSC1Wave, melPhase, fmDt, hermite);
                    melPhaseAdv = fmDt;
                    break;
                }
                case 3: { // PM — phase modulation
                    const float pmOff = osc2 * effectiveIndexF * 5.0f;
                    float pmPh = melPhase + pmOff;
                    pmPh -= floorf(pmPh);
                    melSample = melOscWave(melOSC1Wave, pmPh, melDt, hermite);
                    break;
                }
                default: // AM — amplitude modulation
                    melSample = melCoreSample * (1.0f + osc2 * effectiveIndexF);
                    break;
                }
            }
        } else {
            melSample = 0.0f;
        }

        melOut[i] += melSample * melLvSm * melFadeGain;

        melPhase += melPhaseAdv;
        melPhase -= floorf(melPhase);
        // ---- End melody ----

        // Chord oscillators — ChordAlgo: 0=Mix, 1=FM, 2=PM, 3=AM
        // Carrier waveform = chordWave, modulator waveform = modWave
        float fmPh0 = fp0+fpOff0; fmPh0 -= floorf(fmPh0);
        float fmPh1 = fp1+fpOff1; fmPh1 -= floorf(fmPh1);
        float fmPh2 = fp2+fpOff2; fmPh2 -= floorf(fmPh2);
        float fmPh3 = fp3+fpOff3; fmPh3 -= floorf(fmPh3);
        const float m0r = hermite ? wtReadH(g_wt, fmPh0) : wtRead(g_wt, fmPh0);
        const float m1r = hermite ? wtReadH(g_wt, fmPh1) : wtRead(g_wt, fmPh1);
        const float m2r = hermite ? wtReadH(g_wt, fmPh2) : wtRead(g_wt, fmPh2);
        const float m3r = hermite ? wtReadH(g_wt, fmPh3) : wtRead(g_wt, fmPh3);
        float w0, w1, w2, w3;
        float caDt0 = bdt[0], caDt1 = bdt[1], caDt2 = bdt[2], caDt3 = bdt[3];
        switch (chordAlgo) {
        case 0: { // Mix — equal-power crossfade carrier + modulator
            const float mixF = fmDepthSmoothed * 0.1f;
            const float g1 = cosf(kPi * 0.5f * mixF);
            const float g2 = sinf(kPi * 0.5f * mixF);
            float cPh0 = ph0+phOff0; cPh0 -= floorf(cPh0);
            float cPh1 = ph1+phOff1; cPh1 -= floorf(cPh1);
            float cPh2 = ph2+phOff2; cPh2 -= floorf(cPh2);
            float cPh3 = ph3+phOff3; cPh3 -= floorf(cPh3);
            w0 = wfold((melOscWave(chordWave,cPh0,bdt[0],hermite)*g1 + m0r*g2)*eg0);
            w1 = wfold((melOscWave(chordWave,cPh1,bdt[1],hermite)*g1 + m1r*g2)*eg1);
            w2 = wfold((melOscWave(chordWave,cPh2,bdt[2],hermite)*g1 + m2r*g2)*eg2);
            w3 = wfold((melOscWave(chordWave,cPh3,bdt[3],hermite)*g1 + m3r*g2)*eg3);
            break;
        }
        case 1: { // FM — modulated phase advance
            caDt0 = bdt[0] * (1.0f + m0r * fmDepthSmoothed);
            caDt1 = bdt[1] * (1.0f + m1r * fmDepthSmoothed);
            caDt2 = bdt[2] * (1.0f + m2r * fmDepthSmoothed);
            caDt3 = bdt[3] * (1.0f + m3r * fmDepthSmoothed);
            float cPh0 = ph0+phOff0; cPh0 -= floorf(cPh0);
            float cPh1 = ph1+phOff1; cPh1 -= floorf(cPh1);
            float cPh2 = ph2+phOff2; cPh2 -= floorf(cPh2);
            float cPh3 = ph3+phOff3; cPh3 -= floorf(cPh3);
            w0 = wfold(melOscWave(chordWave,cPh0,caDt0,hermite)*eg0);
            w1 = wfold(melOscWave(chordWave,cPh1,caDt1,hermite)*eg1);
            w2 = wfold(melOscWave(chordWave,cPh2,caDt2,hermite)*eg2);
            w3 = wfold(melOscWave(chordWave,cPh3,caDt3,hermite)*eg3);
            break;
        }
        case 2: { // PM — phase modulation
            const float pmD = fmDepthSmoothed * 0.5f;
            float cPh0 = ph0+phOff0+m0r*pmD; cPh0 -= floorf(cPh0);
            float cPh1 = ph1+phOff1+m1r*pmD; cPh1 -= floorf(cPh1);
            float cPh2 = ph2+phOff2+m2r*pmD; cPh2 -= floorf(cPh2);
            float cPh3 = ph3+phOff3+m3r*pmD; cPh3 -= floorf(cPh3);
            w0 = wfold(melOscWave(chordWave,cPh0,bdt[0],hermite)*eg0);
            w1 = wfold(melOscWave(chordWave,cPh1,bdt[1],hermite)*eg1);
            w2 = wfold(melOscWave(chordWave,cPh2,bdt[2],hermite)*eg2);
            w3 = wfold(melOscWave(chordWave,cPh3,bdt[3],hermite)*eg3);
            break;
        }
        default: { // AM — amplitude modulation
            const float amD = fmDepthSmoothed * 0.1f;
            float cPh0 = ph0+phOff0; cPh0 -= floorf(cPh0);
            float cPh1 = ph1+phOff1; cPh1 -= floorf(cPh1);
            float cPh2 = ph2+phOff2; cPh2 -= floorf(cPh2);
            float cPh3 = ph3+phOff3; cPh3 -= floorf(cPh3);
            w0 = wfold(melOscWave(chordWave,cPh0,bdt[0],hermite)*(1.0f+m0r*amD)*eg0);
            w1 = wfold(melOscWave(chordWave,cPh1,bdt[1],hermite)*(1.0f+m1r*amD)*eg1);
            w2 = wfold(melOscWave(chordWave,cPh2,bdt[2],hermite)*(1.0f+m2r*amD)*eg2);
            w3 = wfold(melOscWave(chordWave,cPh3,bdt[3],hermite)*(1.0f+m3r*amD)*eg3);
            break;
        }
        }
        out[i] = w0*lv0 + w1*lv1 + w2*lv2 + w3*lv3;

        ph0 += caDt0; if (ph0 >= 1.0f) ph0 -= 1.0f;
        ph1 += caDt1; if (ph1 >= 1.0f) ph1 -= 1.0f;
        ph2 += caDt2; if (ph2 >= 1.0f) ph2 -= 1.0f;
        ph3 += caDt3; if (ph3 >= 1.0f) ph3 -= 1.0f;
        fp0 += bdt[0]*fmFreqMult; if (fp0>=1.0f) fp0-=1.0f; else if (fp0<0.0f) fp0+=1.0f;
        fp1 += bdt[1]*fmFreqMult; if (fp1>=1.0f) fp1-=1.0f; else if (fp1<0.0f) fp1+=1.0f;
        fp2 += bdt[2]*fmFreqMult; if (fp2>=1.0f) fp2-=1.0f; else if (fp2<0.0f) fp2+=1.0f;
        fp3 += bdt[3]*fmFreqMult; if (fp3>=1.0f) fp3-=1.0f; else if (fp3<0.0f) fp3+=1.0f;
    }

    // Write-back
    d->prevAdvTrig   = prevTrig;
    d->prevResetTrig = prevReset;
    d->lfoPhase1 = lfoPhase1; d->lfoPhase2 = lfoPhase2;
    d->lfo1S = lfo1S; d->lfo1C = lfo1C;
    d->lfo2S = lfo2S; d->lfo2C = lfo2C;
    d->osc_phase[0]=ph0; d->osc_phase[1]=ph1; d->osc_phase[2]=ph2; d->osc_phase[3]=ph3;
    d->fmPhase[0]=fp0;   d->fmPhase[1]=fp1;   d->fmPhase[2]=fp2;   d->fmPhase[3]=fp3;
    d->osc_freq[0]=f0;   d->osc_freq[1]=f1;   d->osc_freq[2]=f2;   d->osc_freq[3]=f3;
    d->bassPhase=bassPhase; d->bassFreq=bassFreq; d->bassTargetFreq=bassTargetFreq;
    d->bassGlideStartFreq=bassGlideStartFreq; d->bassGlideProgress=bassGlideProgress;
    d->bassGlideDT=bassGlideDT; d->prevClockTrig=prevClockTrig; d->prevBassSample=prevBassSample;
    a->bassGated=bassGated; a->bassPendingGated=bassPendingGated; a->bassHasPending=bassHasPending;
    a->bassLoopPos=bassLoopPos; a->bassNoteUpdateCount=bassNoteUpdateCount;
    a->bassHasPendingNote=bassHasPendingNote; a->bassNoteUpdateTargetHz=bassNoteUpdateTargetHz;
    d->melPhase=melPhase; d->melFreq=melFreq; d->melTargetFreq=melTargetFreq;
    d->melGlideStartFreq=melGlideStartFreq; d->melGlideProgress=melGlideProgress;
    d->melGlideDT=melGlideDT; d->prevMelClockTrig=prevMelClockTrig; d->prevMelSample=prevMelSample;
    d->mel2Phase=mel2Phase;
    for (int i = 0; i < 7; ++i) d->melSwarmPhase[i] = swPh[i];
    d->melLFOPhase=melLFOPhase;
    d->melFadeGain=melFadeGain;
    a->melGated=melGated; a->melPendingGated=melPendingGated; a->melHasPending=melHasPending;
    a->melLoopPos=melLoopPos; a->melNoteUpdateCount=melNoteUpdateCount;
    a->melHasPendingNote=melHasPendingNote; a->melNoteUpdateTargetHz=melNoteUpdateTargetHz;
    d->fmDepthSmoothed  = fmDepthSmoothed;
    d->melIndexSmoothed = melIndexSmoothed;
    d->lv0Smoothed = lv0Sm; d->lv1Smoothed = lv1Sm;
    d->lv2Smoothed = lv2Sm; d->lv3Smoothed = lv3Sm;
    d->bassLevelSmoothed = bassLvSm;
    d->melLevelSmoothed  = melLvSm;
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
    .calculateStaticRequirements = calculateStaticRequirements,
    .initialise                  = initialise,
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
