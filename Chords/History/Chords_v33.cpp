#include "distingnt/api.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

// ---------------------------------------------------------------------------
// Chord data
// ---------------------------------------------------------------------------

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
    {  0,  0,  7,   7 }, // 17 5th
};

static const float kC0Hz = 130.8128f / 8.0f;
static const float kPi   = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Wavetable constants (melody oscillators only — there is no chord voice)
// ---------------------------------------------------------------------------
#define WT_SIZE    2048
#define WT_MASK    (WT_SIZE - 1)
#define WT_MIPS    9
#define WT_NTABLES (2 + 2 * WT_MIPS)

static const int kWTMipH[WT_MIPS] = { 512, 256, 128, 64, 32, 16, 8, 4, 2 };

static float* g_wt = nullptr;

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

enum {
    kParamInv1 = 0, kParamDeg1,   //  0,  1
    kParamInv2,     kParamDeg2,   //  2,  3
    kParamInv3,     kParamDeg3,   //  4,  5
    kParamInv4,     kParamDeg4,   //  6,  7
    kParamInv5,     kParamDeg5,   //  8,  9
    kParamInv6,     kParamDeg6,   // 10, 11
    kParamInv7,     kParamDeg7,   // 12, 13
    kParamInv8,     kParamDeg8,   // 14, 15
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
    kParamChordAlgo,   // 37
    kParamOctave,      // 38  <- compatibility placeholder (was in v20)
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
    kParamMelOSC2Wave,   // 64
    kParamMelAlgo,       // 65
    kParamMelIndex,      // 66
    kParamMelOSC2Partial,// 67
    kParamMelOSC2Semi,   // 68
    kParamMelDetune,     // 69
    kParamAmpLFOShape,   // 70
    kParamShpLFOShape,   // 71
    kParamResetOnChrd,   // 72
    kParamMelLFOSpeed,   // 73
    kParamMelLFOShape,   // 74
    kParamMelLFOReset,   // 75
    kParamMelLFOAmt,     // 76
    kParamScale,         // 77  (was kParamGenre=77 in v20)
    kParamGenProgLen,    // 78
    kParamGenerate,      // 79
    kParamGenRoot,       // 80
    kParamVoiceLeading,  // 81  <- compatibility placeholder (was in v20)
    kParamBassOctave,    // 82
    kParamChordWave,     // 83
    kParamModWave,       // 84
    kParamHermite,       // 85
    kParamBassDetune,    // 86
    kParamBass2Level,    // 87
    kParamAdvance,       // 88
    kParamInversion,     // 89  <- new global inversion
    // MIDI page (appended to preserve all existing mappings)
    kParamBassCH,        // 90
    kParamMelCH,         // 91
    kParamChord1CH,      // 92
    kParamChord2CH,      // 93
    kParamChord3CH,      // 94
    kParamChord4CH,      // 95
    kParamChordDensity,  // 96
    kParamMIDIOut,       // 97
    kParamChordOctave,   // 98  <- shifts the Chord1-4 CH MIDI notes by octaves
    kParamBassSnap,      // 99  0=Scale 1=Chord
    kParamMelSnap,       // 100 0=Scale 1=Chord
    kNumParams           // 101
};

#define INV_PARAM(n) \
    { .name="Inv " #n, .min=0, .max=3, .def=0, \
      .unit=kNT_unitNone, .scaling=0, .enumStrings=NULL },
#define DEG_PARAM(n) \
    { .name="Degree " #n, .min=0, .max=6, .def=0, \
      .unit=kNT_unitEnum, .scaling=0, .enumStrings=degreeNames },

static const char* const degreeNames[]       = { "I","II","III","IV","V","VI","VII", NULL };
static const char* const scaleNames[]        = {
    "Major","Nat Minor","Dorian","Mixolydian","Lydian",
    "Harm Min","Mel Min","Dbl Harm","Hung Min","Hirajoshi", NULL };
static const char* const bassWaveformNames[] = { "Sine", "Tri", "Saw", "Square", NULL };
static const char* const advanceNames[]      = { "0", "1", NULL };
static const char* const melWaveNames[]      = { "Sine", "Tri", "Saw", "Square", NULL };
static const char* const melAlgoNames[]      = { "Mix", "FM", "Swarm", "PM", "AM", NULL };
static const char* const chordAlgoNames[]    = { "Mix", "FM", "PM", "AM", NULL };
static const char* const lfoShapeNames[]     = { "sine", "saw", "ramp", "square", NULL };
static const char* const genRootNames[]      = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", NULL };
static const char* const midiOutNames[]      = { "DIN", "USB", NULL };
static const char* const snapNames[]         = { "Scale", "Chord", NULL };

#define SHAPE_PARAM(n) \
    { .name="OSC" #n " Shape", .min=0, .max=1000, .def=0, \
      .unit=kNT_unitNone, .scaling=0, .enumStrings=NULL },

static const _NT_parameter parameters[kNumParams] = {
    INV_PARAM(1) DEG_PARAM(1)    //  0,  1
    INV_PARAM(2) DEG_PARAM(2)    //  2,  3
    INV_PARAM(3) DEG_PARAM(3)    //  4,  5
    INV_PARAM(4) DEG_PARAM(4)    //  6,  7
    INV_PARAM(5) DEG_PARAM(5)    //  8,  9
    INV_PARAM(6) DEG_PARAM(6)    // 10, 11
    INV_PARAM(7) DEG_PARAM(7)    // 12, 13
    INV_PARAM(8) DEG_PARAM(8)    // 14, 15
    // 16
    { .name="ProgLength", .min=1, .max=8, .def=4,
      .unit=kNT_unitNone, .scaling=0, .enumStrings=NULL },
    // 17
    NT_PARAMETER_CV_INPUT( "AdvanceIn", 0, 0 )
    // 18
    NT_PARAMETER_CV_INPUT( "Reset", 0, 0 )
    // 19
    { .name="ProgDiv", .min=1, .max=128, .def=1,
      .unit=kNT_unitNone, .scaling=0, .enumStrings=NULL },
    // 20
    { .name="OSC1 Level", .min=0, .max=100, .def=25,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    SHAPE_PARAM(1)   // 21
    // 22
    { .name="OSC2 Level", .min=0, .max=100, .def=25,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    SHAPE_PARAM(2)   // 23
    // 24
    { .name="OSC3 Level", .min=0, .max=100, .def=25,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    SHAPE_PARAM(3)   // 25
    // 26
    { .name="OSC4 Level", .min=0, .max=100, .def=25,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
    SHAPE_PARAM(4)   // 27
    // 28
    { .name="Detune",    .min=0,  .max=33,  .def=0,
      .unit=kNT_unitCents,   .scaling=0,            .enumStrings=NULL },
    // 29
    NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )
    // 30
    { .name="LevelFreq", .min=1,   .max=2000, .def=100,
      .unit=kNT_unitHz,      .scaling=kNT_scaling100, .enumStrings=NULL },
    // 31
    { .name="LevelMod",  .min=0,   .max=100,  .def=0,
      .unit=kNT_unitPercent, .scaling=0,              .enumStrings=NULL },
    // 32
    { .name="ShapeFreq", .min=1,   .max=2000, .def=100,
      .unit=kNT_unitHz,      .scaling=kNT_scaling100, .enumStrings=NULL },
    // 33
    { .name="ShapeMod",  .min=0,   .max=100,  .def=0,
      .unit=kNT_unitPercent, .scaling=0,              .enumStrings=NULL },
    // 34
    { .name="FMFreq",    .min=-4,  .max=16,   .def=1,
      .unit=kNT_unitNone,    .scaling=0,              .enumStrings=NULL },
    // 35
    { .name="ChordIndex",.min=0,   .max=1000, .def=0,
      .unit=kNT_unitPercent, .scaling=kNT_scaling10,  .enumStrings=NULL },
    // 36
    { .name="Phase",     .min=0,   .max=3600, .def=0,
      .unit=kNT_unitDegrees, .scaling=kNT_scaling10,  .enumStrings=NULL },
    // 37
    { .name="ChordAlgo", .min=0,   .max=3,    .def=0,
      .unit=kNT_unitEnum,    .scaling=0,              .enumStrings=chordAlgoNames },
    // 38 - compatibility placeholder (kParamOctave, was present in v20)
    { .name="Octave",    .min=-2,  .max=2,    .def=0,
      .unit=kNT_unitNone,    .scaling=0,              .enumStrings=NULL },
    // 39 Bass page
    { .name="BassWave",    .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=bassWaveformNames },
    // 40
    { .name="BassLevel",   .min=0,  .max=100, .def=50,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
    // 41
    NT_PARAMETER_AUDIO_OUTPUT( "BassOut", 1, 14 )
    // 42
    NT_PARAMETER_CV_INPUT( "ClockIn", 0, 0 )
    // 43
    { .name="BassRange",   .min=0,  .max=15,  .def=7,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 44
    { .name="BassOffset",  .min=0,  .max=3,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 45
    { .name="Randomise",   .min=0,  .max=1,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 46
    { .name="LoopLength",  .min=1,  .max=16,  .def=8,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 47
    { .name="Glide",       .min=0,  .max=100, .def=0,
      .unit=kNT_unitSeconds, .scaling=kNT_scaling100,  .enumStrings=NULL },
    // 48
    { .name="BassDensity", .min=0,  .max=100, .def=75,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
    // 49
    { .name="GlideCurve",  .min=0,  .max=100, .def=50,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 50
    { .name="NoteUpdate",  .min=0,  .max=8,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 51 Melody page
    { .name="OSC1 Wave",   .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=melWaveNames },
    // 52
    { .name="MelLevel",    .min=0,  .max=100, .def=50,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
    // 53
    NT_PARAMETER_AUDIO_OUTPUT( "MelOut", 1, 15 )
    // 54
    NT_PARAMETER_CV_INPUT( "ClockIn.", 0, 0 )
    // 55
    { .name="MelRange",    .min=0,  .max=15,  .def=7,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 56
    { .name="MelOffset",   .min=-4, .max=3,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 57
    { .name="Randomise.",  .min=0,  .max=1,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 58
    { .name="LoopLength.", .min=1,  .max=16,  .def=8,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 59
    { .name="Glide.",      .min=0,  .max=100, .def=0,
      .unit=kNT_unitSeconds, .scaling=kNT_scaling100,  .enumStrings=NULL },
    // 60
    { .name="MelDensity",  .min=0,  .max=100, .def=75,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
    // 61
    { .name="GlideCurve.", .min=0,  .max=100, .def=50,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 62
    { .name="NoteUpdate.", .min=0,  .max=8,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 63
    { .name="MelOctave",   .min=-4, .max=4,    .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 64 Melody synth
    { .name="OSC2 Wave",   .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=melWaveNames },
    // 65
    { .name="Algo",        .min=0,  .max=4,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=melAlgoNames },
    // 66
    { .name="Index",       .min=0,  .max=1000,.def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 67
    { .name="OSC2 Part",   .min=0,  .max=12,  .def=1,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 68
    { .name="OSC2 Semi",   .min=-24,.max=24,  .def=0,
      .unit=kNT_unitSemitones,.scaling=0,              .enumStrings=NULL },
    // 69
    { .name="MelDetune",   .min=-50,.max=50,  .def=0,
      .unit=kNT_unitCents,   .scaling=0,               .enumStrings=NULL },
    // 70 QuadLFO extensions
    { .name="AmpLFOShape", .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=lfoShapeNames },
    // 71
    { .name="ShpLFOShape", .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=lfoShapeNames },
    // 72
    { .name="ResetOnChrd", .min=0,  .max=1,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 73 Melody LFO
    { .name="MelLFOSpeed", .min=1,  .max=2000,.def=100,
      .unit=kNT_unitHz,      .scaling=kNT_scaling100,  .enumStrings=NULL },
    // 74
    { .name="MelLFOShape", .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=lfoShapeNames },
    // 75
    { .name="MelLFOReset", .min=0,  .max=1,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 76
    { .name="MelLFOAmt",  .min=0,  .max=100, .def=0,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
    // 77 Generator page (same index as kParamGenre in v20)
    { .name="Scale",      .min=0,  .max=9,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=scaleNames },
    // 78
    { .name="ProgLength", .min=1,  .max=8,   .def=4,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 79
    { .name="Generate!",  .min=0,  .max=1,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 80
    { .name="Root",       .min=0,  .max=11,  .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=genRootNames },
    // 81 - compatibility placeholder (kParamVoiceLeading, was in v20)
    { .name="VoiceLeading",.min=0,  .max=3,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 82
    { .name="BassOctave", .min=-2,  .max=2,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 83
    { .name="ChordWave", .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=melWaveNames },
    // 84
    { .name="ModWave",   .min=0,  .max=3,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=melWaveNames },
    // 85
    { .name="Hermite",   .min=0,  .max=1,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 86
    { .name="BassDetune",.min=-50,.max=50,  .def=0,
      .unit=kNT_unitCents,   .scaling=0,               .enumStrings=NULL },
    // 87
    { .name="Bass2Level",.min=0,  .max=100, .def=0,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
    // 88
    { .name="Advance",   .min=0,  .max=1,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=advanceNames },
    // 89 - new global inversion parameter
    { .name="Inversion", .min=0,  .max=3,   .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 90 MIDI page
    { .name="BassCH",    .min=0,  .max=16,  .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 91
    { .name="MelCH",     .min=0,  .max=16,  .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 92
    { .name="Chord1CH",  .min=0,  .max=16,  .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 93
    { .name="Chord2CH",  .min=0,  .max=16,  .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 94
    { .name="Chord3CH",  .min=0,  .max=16,  .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 95
    { .name="Chord4CH",  .min=0,  .max=16,  .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 96
    { .name="ChordDensity",.min=0,.max=100, .def=75,
      .unit=kNT_unitPercent, .scaling=0,               .enumStrings=NULL },
    // 97
    { .name="MIDIOut",   .min=0,  .max=1,   .def=0,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=midiOutNames },
    // 98
    { .name="ChordOctave", .min=-2, .max=2, .def=0,
      .unit=kNT_unitNone,    .scaling=0,               .enumStrings=NULL },
    // 99
    { .name="BassSnap", .min=0, .max=1, .def=1,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=snapNames },
    // 100
    { .name="MelSnap",  .min=0, .max=1, .def=1,
      .unit=kNT_unitEnum,    .scaling=0,               .enumStrings=snapNames },
};

// ---------------------------------------------------------------------------
// Parameter pages
// ---------------------------------------------------------------------------

// Progression page: only Degree params (Inv params kept as hidden placeholders)
static const uint8_t progressionPageParams[] = {
    kParamDeg1,
    kParamDeg2,
    kParamDeg3,
    kParamDeg4,
    kParamDeg5,
    kParamDeg6,
    kParamDeg7,
    kParamDeg8,
    kParamProgLength,
    kParamAdvanceIn,
    kParamResetIn,
    kParamProgDiv,
    kParamAdvance,
};

// There is no more Chord or QuadLFO page — the chord voice is now purely
// the Chord1-4 CH MIDI stream (see updateFrequencies() / step()), driven by
// the progression + Inversion. kParamOutput, kParamChordOctave, and all the
// former OSC1-4 Level/Shape/Detune/FMFreq/ChordIndex/ChordAlgo/ChordWave/
// Hermite/Phase/QuadLFO parameters are kept in parameters[] below but on no
// page — inert placeholders so existing CV/MIDI mappings and parameter
// indices stay valid. kParamChordOctave itself is still applied in
// updateFrequencies() (it shifts the MIDI notes), just not shown anywhere.

static const uint8_t bassPageParams[] = {
    kParamBassWaveform,
    kParamBassLevel,
    kParamBassDetune,
    kParamBass2Level,
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
    kParamBassSnap,
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
    kParamMelSnap,
};

// Generator page includes the new global Inversion parameter
static const uint8_t generatorPageParams[] = {
    kParamScale,
    kParamGenRoot,
    kParamGenProgLen,
    kParamInversion,
    kParamGenerate,
};

// MIDI page — channels, densities, output port
static const uint8_t midiPageParams[] = {
    kParamBassCH,
    kParamMelCH,
    kParamChord1CH,
    kParamChord2CH,
    kParamChord3CH,
    kParamChord4CH,
    kParamChordDensity,
    kParamMelDensity,    // shared with Melody page — controls MIDI rhythm for melody
    kParamMIDIOut,
};

static const _NT_parameterPage pages[] = {
    { .name="Progression", .numParams=ARRAY_SIZE(progressionPageParams), .group=0, .params=progressionPageParams },
    { .name="Bass",         .numParams=ARRAY_SIZE(bassPageParams),         .group=0, .params=bassPageParams },
    { .name="Melody",       .numParams=ARRAY_SIZE(melodyPageParams),       .group=0, .params=melodyPageParams },
    { .name="Generator",    .numParams=ARRAY_SIZE(generatorPageParams),    .group=0, .params=generatorPageParams },
    { .name="MIDI",         .numParams=ARRAY_SIZE(midiPageParams),         .group=0, .params=midiPageParams },
};

static const _NT_parameterPages paramPages = {
    .numPages = 5,
    .pages    = pages,
};

// ---------------------------------------------------------------------------
// Algorithm structs
// ---------------------------------------------------------------------------

struct ChordsAlg_DTC {
    float osc_freq[4];      // 4 chord voice pitches — Chord1-4 CH MIDI reporting only, no audio voice
    float osc_levelV[4];    // unused; kept only so the OSC1-4 Level placeholders have somewhere to write
    float prevAdvTrig;
    float prevResetTrig;
    float bassPhase;
    float bassFreq;
    float bassTargetFreq;
    float bassGlideStartFreq;
    float bassGlideProgress;
    float bassGlideDT;
    float bassLevelV;
    float prevClockTrig;
    float prevBassSample;
    float prevBass2Sample;
    float bass2Phase;
    float bass2LevelV;
    float bass2LevelSmoothed;
    float melPhase;
    float melFreq;
    float melTargetFreq;
    float melGlideStartFreq;
    float melGlideProgress;
    float melGlideDT;
    float melLevelV;
    float prevMelClockTrig;
    float prevMelSample;
    float mel2Phase;
    float melSwarmPhase[7];
    float melLFOPhase;
    float melFadeGain;
    float fmDepthSmoothed;
    float melIndexSmoothed;
    float bassLevelSmoothed;
    float melLevelSmoothed;
};

struct ChordsAlg : public _NT_algorithm {
    ChordsAlg_DTC* dtc;
    int currentStep;
    int   advDivCount;
    float levelModDepth;
    float shapeModDepth;
    float osc_gainF[4];
    float fmFreqMult;
    float fmDepth;
    float phaseStep;
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
    bool    bass2Gated;
    bool    bass2PendingGated;
    bool    bass2HasPending;
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
    float   melOSC2FreqScale;
    float   melSwarmRatio[7];
    float   bassOctaveScale;
    float   bass2FreqRatio;
    uint32_t rngState;
    // MIDI output state
    int     bassMidiNote;           // last sent note, -1 = none
    int     melMidiNote;            // last sent note, -1 = none
    int     chordMidiNotes[4];      // current chord MIDI notes, -1 = none
    bool    chordMidiGated;         // whether chord MIDI notes are currently sounding
    int     chordLoopPos;           // position in chordDensityLoop
    float   chordDensityLoop[16];   // random density values for chord rhythm (like bass densityLoop)
    bool    pendingChordMidiUpdate; // set by updateFrequencies(), consumed at top of step()
    // Melody-page shift (Ch5 Note 62): both OSC2 Semi and OSC2 Part can be
    // hardware/firmware MIDI-mapped to the same CC (Ch5 CC21). Shift decides
    // which one actually accepts that CC's edits; the other's edit is reverted.
    bool    melShiftHeld;
    int     melOSC2SemiPrev;        // last accepted OSC2 Semi value, restored when Semi is blocked
    int     melOSC2PartialPrev;     // last accepted OSC2 Part value, restored when Part is blocked

    ChordsAlg(ChordsAlg_DTC* d) : dtc(d), currentStep(0), advDivCount(0),
                                   levelModDepth(0.0f), shapeModDepth(0.0f),
                                   fmFreqMult(1.0f), fmDepth(0.0f), phaseStep(0.0f),
                                   bassLoopPos(0), bassGated(false),
                                   bassPendingGated(false), bassHasPending(false),
                                   bassNoteUpdateCount(0), bassHasPendingNote(false),
                                   bassNoteUpdateTargetHz(0.0f),
                                   bass2Gated(false), bass2PendingGated(false), bass2HasPending(false),
                                   melLoopPos(0), melGated(false),
                                   melPendingGated(false), melHasPending(false),
                                   melNoteUpdateCount(0), melHasPendingNote(false),
                                   melNoteUpdateTargetHz(0.0f),
                                   melOctaveShift(1.0f),
                                   melOSC2FreqScale(1.0f),
                                   bassOctaveScale(1.0f),
                                   bass2FreqRatio(1.0f),
                                   rngState(0x12345678u),
                                   bassMidiNote(-1), melMidiNote(-1),
                                   chordMidiGated(false), chordLoopPos(0),
                                   pendingChordMidiUpdate(false),
                                   melShiftHeld(false), melOSC2SemiPrev(0),
                                   melOSC2PartialPrev(1) {
        osc_gainF[0] = osc_gainF[1] = osc_gainF[2] = osc_gainF[3] = 1.0f;
        for (int i = 0; i < 4; ++i) chordMidiNotes[i] = -1;
        for (int i = 0; i < 16; ++i) chordDensityLoop[i] = 0.5f;
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

static inline void posWave4(int shape, float phase,
                            float* w0, float* w1, float* w2, float* w3) {
    switch (shape) {
    default:
    case 0: {
        const float s = sinf(2.0f * kPi * phase);
        const float c = cosf(2.0f * kPi * phase);
        *w0 = 0.5f * (1.0f + s);
        *w1 = 0.5f * (1.0f + c);
        *w2 = 0.5f * (1.0f - s);
        *w3 = 0.5f * (1.0f - c);
        break;
    }
    case 1: {
        float p1 = phase + 0.25f; if (p1 >= 1.0f) p1 -= 1.0f;
        float p2 = phase + 0.50f; if (p2 >= 1.0f) p2 -= 1.0f;
        float p3 = phase + 0.75f; if (p3 >= 1.0f) p3 -= 1.0f;
        *w0 = phase; *w1 = p1; *w2 = p2; *w3 = p3;
        break;
    }
    case 2: {
        float p1 = phase + 0.25f; if (p1 >= 1.0f) p1 -= 1.0f;
        float p2 = phase + 0.50f; if (p2 >= 1.0f) p2 -= 1.0f;
        float p3 = phase + 0.75f; if (p3 >= 1.0f) p3 -= 1.0f;
        *w0 = 1.0f - phase; *w1 = 1.0f - p1; *w2 = 1.0f - p2; *w3 = 1.0f - p3;
        break;
    }
    case 3: {
        *w0 = phase < 0.5f ? 1.0f : 0.0f;
        float p1 = phase + 0.25f; if (p1 >= 1.0f) p1 -= 1.0f; *w1 = p1 < 0.5f ? 1.0f : 0.0f;
        float p2 = phase + 0.50f; if (p2 >= 1.0f) p2 -= 1.0f; *w2 = p2 < 0.5f ? 1.0f : 0.0f;
        float p3 = phase + 0.75f; if (p3 >= 1.0f) p3 -= 1.0f; *w3 = p3 < 0.5f ? 1.0f : 0.0f;
        break;
    }
    }
}

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

void calculateStaticRequirements(_NT_staticRequirements& req) {
    req.dram = (uint32_t)(WT_NTABLES * WT_SIZE * sizeof(float));
}

static inline void wtAddHarmonic(float* tbl, int n, float amp) {
    const float dx   = 2.0f * kPi * (float)n / (float)WT_SIZE;
    const float cosD = cosf(dx);
    const float sinD = sinf(dx);
    float s = 0.0f, c = 1.0f;
    for (int i = 0; i < WT_SIZE; ++i) {
        tbl[i] += amp * s;
        const float ns = s * cosD + c * sinD;
        c = c * cosD - s * sinD;
        s = ns;
    }
}

void initialise(_NT_staticMemoryPtrs& ptrs, const _NT_staticRequirements&) {
    g_wt = (float*)ptrs.dram;

    float* tSine = g_wt;
    for (int i = 0; i < WT_SIZE; ++i)
        tSine[i] = sinf(2.0f * kPi * (float)i / (float)WT_SIZE);

    float* tTri = g_wt + WT_SIZE;
    memset(tTri, 0, WT_SIZE * sizeof(float));
    for (int k = 0; (2*k+1) <= WT_SIZE/2; ++k) {
        const int   n   = 2*k + 1;
        const float amp = (8.0f / (kPi * kPi * (float)(n*n))) * ((k & 1) ? -1.0f : 1.0f);
        wtAddHarmonic(tTri, n, amp);
    }

    for (int m = 0; m < WT_MIPS; ++m) {
        float* tSaw = g_wt + (2 + m) * WT_SIZE;
        memset(tSaw, 0, WT_SIZE * sizeof(float));
        const int N = kWTMipH[m];
        for (int n = 1; n <= N; ++n)
            wtAddHarmonic(tSaw, n, -2.0f / (kPi * (float)n));
    }

    for (int m = 0; m < WT_MIPS; ++m) {
        float* tSqr = g_wt + (2 + WT_MIPS + m) * WT_SIZE;
        memset(tSqr, 0, WT_SIZE * sizeof(float));
        const int N = kWTMipH[m];
        for (int n = 1; n <= N; n += 2)
            wtAddHarmonic(tSqr, n, 4.0f / (kPi * (float)n));
    }
}

static inline float wtRead(const float* tbl, float phase) {
    const float pos  = phase * (float)WT_SIZE;
    const int   i0   = (int)pos & WT_MASK;
    const float frac = pos - (float)i0;
    return tbl[i0] + frac * (tbl[(i0 + 1) & WT_MASK] - tbl[i0]);
}

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

static inline int wtMip(float dt) {
    if (dt <= 0.0f) return 0;
    const float maxH = 0.5f / dt;
    for (int m = 0; m < WT_MIPS; ++m)
        if ((float)kWTMipH[m] <= maxH) return m;
    return WT_MIPS - 1;
}

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

// ---------------------------------------------------------------------------
// Scale data
// ---------------------------------------------------------------------------

// 10 scales: 0=Major  1=NatMin  2=Dorian  3=Mixolydian  4=Lydian
//            5=HarmMin  6=MelMin  7=DblHarm  8=HungMin  9=Hirajoshi
static const int8_t kScaleIntervals[10][7] = {
    { 0, 2, 4, 5, 7, 9, 11 }, // Major
    { 0, 2, 3, 5, 7, 8, 10 }, // Natural Minor
    { 0, 2, 3, 5, 7, 9, 10 }, // Dorian
    { 0, 2, 4, 5, 7, 9, 10 }, // Mixolydian
    { 0, 2, 4, 6, 7, 9, 11 }, // Lydian
    { 0, 2, 3, 5, 7, 8, 11 }, // Harmonic Minor
    { 0, 2, 3, 5, 7, 9, 11 }, // Melodic Minor
    { 0, 1, 4, 5, 7, 8, 11 }, // Double Harmonic
    { 0, 2, 3, 6, 7, 8, 11 }, // Hungarian Minor
    { 0, 2, 3, 7, 8, 10, 11 }, // Hirajoshi (pentatonic extended)
};

// Chord types per scale degree: 4=maj7  5=min7  3=dom7  10=m7b5  11=dim7
static const uint8_t kScaleChordTypes[10][7] = {
    {  4,  5,  5,  4,  3,  5, 10 }, // Major
    {  5, 10,  4,  5,  3,  4,  3 }, // Natural Minor
    {  5,  5,  4,  3,  5, 10,  4 }, // Dorian
    {  3,  5, 10,  4,  5,  5,  4 }, // Mixolydian
    {  4,  3,  5, 10,  4,  5,  5 }, // Lydian
    {  5, 10,  4,  5,  3,  4, 11 }, // Harmonic Minor
    {  5,  5,  4,  3,  3, 10, 10 }, // Melodic Minor
    {  4,  4,  3,  5,  3,  4, 11 }, // Double Harmonic
    {  5, 10,  4, 11,  3,  4, 11 }, // Hungarian Minor
    {  5, 10,  4,  5,  4,  5, 11 }, // Hirajoshi
};

// Harmonic functions: 0=Tonic  1=Subdominant  2=Dominant
static const uint8_t kScaleFunctions[10][7] = {
    { 0, 1, 0, 1, 2, 0, 2 }, // Major
    { 0, 1, 0, 1, 2, 1, 1 }, // Natural Minor
    { 0, 1, 0, 1, 1, 1, 0 }, // Dorian
    { 0, 1, 1, 1, 1, 0, 2 }, // Mixolydian
    { 0, 1, 0, 1, 0, 1, 1 }, // Lydian
    { 0, 1, 0, 1, 2, 1, 2 }, // Harmonic Minor
    { 0, 1, 0, 1, 2, 1, 2 }, // Melodic Minor
    { 0, 2, 1, 1, 2, 1, 2 }, // Double Harmonic
    { 0, 1, 0, 2, 2, 1, 2 }, // Hungarian Minor
    { 0, 1, 0, 1, 2, 1, 2 }, // Hirajoshi
};

static inline uint32_t genLcgNext(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s;
}
static inline int genLcgRange(uint32_t& s, int n) {
    if (n <= 1) return 0;
    return (int)((genLcgNext(s) >> 16) % (uint32_t)n);
}

// ---------------------------------------------------------------------------
// MIDI helpers
// ---------------------------------------------------------------------------

static inline int hzToMidi(float hz) {
    if (hz <= 0.0f) return -1;
    int n = (int)roundf(69.0f + 12.0f * logf(hz / 440.0f) * 1.44269504f);
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    return n;
}

// ---------------------------------------------------------------------------
// Note pool helpers
// ---------------------------------------------------------------------------

static void updateNotePool(ChordsAlg* a) {
    const int step      = a->currentStep;
    const int deg       = a->v[kParamDeg1 + step * 2];
    const int scaleIdx  = a->v[kParamScale];
    const int genRoot   = a->v[kParamGenRoot];
    const int chordType = kScaleChordTypes[scaleIdx][deg];
    const int noteClass = (genRoot + (int)kScaleIntervals[scaleIdx][deg]) % 12;
    // Root fixed at octave 3
    const float rootHz  = kC0Hz * powf(2.0f, (float)(36 + noteClass) / 12.0f);
    int np = 0;
    if (a->v[kParamBassSnap] == 0) {
        // Scale mode: 16 consecutive scale degrees from the root — the
        // progression no longer steers this voice; it plays freely within
        // Root + Scale.
        const float keyHz = kC0Hz * powf(2.0f, (float)(36 + genRoot) / 12.0f) * 0.5f;
        for (np = 0; np < 16; ++np) {
            const int sd  = np % 7;
            const int oct = np / 7;
            a->notePool[np] = keyHz *
                powf(2.0f, ((float)kScaleIntervals[scaleIdx][sd] + 12.0f * oct) / 12.0f);
        }
    } else {
        // Chord mode: the current chord's 4 tones across 4 octaves.
        for (int i = 0; i < 4; ++i) {
            int semis = (int)chordIntervals[chordType][i];
            if (semis < 0) semis += 12;
            const float baseHz = rootHz * powf(2.0f, (float)semis / 12.0f);
            for (int oct = -2; oct <= 1; ++oct)
                a->notePool[np++] = baseHz * powf(2.0f, (float)oct);
        }
    }
    for (int i = 1; i < 16; ++i) {
        const float key = a->notePool[i];
        int j = i - 1;
        while (j >= 0 && a->notePool[j] > key) { a->notePool[j+1] = a->notePool[j]; --j; }
        a->notePool[j+1] = key;
    }
}

static void updateMelNotePool(ChordsAlg* a) {
    const int step      = a->currentStep;
    const int deg       = a->v[kParamDeg1 + step * 2];
    const int scaleIdx  = a->v[kParamScale];
    const int genRoot   = a->v[kParamGenRoot];
    const int chordType = kScaleChordTypes[scaleIdx][deg];
    const int noteClass = (genRoot + (int)kScaleIntervals[scaleIdx][deg]) % 12;
    const float rootHz  = kC0Hz * powf(2.0f, (float)(36 + noteClass) / 12.0f);
    const int octOff    = a->v[kParamMelOctave];
    a->melOctaveShift   = powf(2.0f, (float)octOff);
    int np = 0;
    if (a->v[kParamMelSnap] == 0) {
        // Scale mode: 16 consecutive scale degrees from the root, in the
        // melody's base register — the progression no longer steers this
        // voice; it plays freely within Root + Scale.
        const float keyHz = kC0Hz * powf(2.0f, (float)(36 + genRoot) / 12.0f) * powf(2.0f, 2.0f + (float)octOff);
        for (np = 0; np < 16; ++np) {
            const int sd  = np % 7;
            const int oct = np / 7;
            a->melNotePool[np] = keyHz *
                powf(2.0f, ((float)kScaleIntervals[scaleIdx][sd] + 12.0f * oct) / 12.0f);
        }
    } else {
        // Chord mode: the current chord's 4 tones across 4 octaves.
        for (int i = 0; i < 4; ++i) {
            int semis = (int)chordIntervals[chordType][i];
            if (semis < 0) semis += 12;
            const float baseHz = rootHz * powf(2.0f, (float)semis / 12.0f);
            for (int oct = 2 + octOff; oct <= 5 + octOff; ++oct)
                a->melNotePool[np++] = baseHz * powf(2.0f, (float)oct);
        }
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
    const int step      = a->currentStep;
    const int deg       = a->v[kParamDeg1 + step * 2];
    const int scaleIdx  = a->v[kParamScale];
    const int genRoot   = a->v[kParamGenRoot];
    const int chordType = kScaleChordTypes[scaleIdx][deg];
    const int noteClass = (genRoot + (int)kScaleIntervals[scaleIdx][deg]) % 12;
    // Root fixed at octave 3
    const float rootHz  = kC0Hz * powf(2.0f, (float)(36 + noteClass) / 12.0f);
    const float chordOctaveScale = powf(2.0f, (float)a->v[kParamChordOctave]);
    // There is no chord audio voice — d->osc_freq[] exists purely to drive
    // the Chord1-4 CH MIDI output below. Chord type now follows the current
    // scale degree via kScaleChordTypes, same as updateNotePool()/
    // updateMelNotePool(), instead of being hard-coded to min7. Negative
    // table entries (-12) are left un-normalised on purpose — they
    // intentionally voice that tone an octave down.
    for (int i = 0; i < 4; ++i)
        d->osc_freq[i] = rootHz * powf(2.0f, (float)chordIntervals[chordType][i] / 12.0f) * chordOctaveScale;
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
                d->bassFreq          = closestHz;
                d->bassGlideProgress = 1.0f;
                d->bassGlideDT       = 0.0f;
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
                d->melFreq          = closestHz;
                d->melGlideProgress = 1.0f;
                d->melGlideDT       = 0.0f;
            } else {
                d->melGlideStartFreq = d->melFreq;
                d->melGlideProgress  = 0.0f;
                const float samps = glideSecs * NT_globals.sampleRate;
                d->melGlideDT = (samps > 1.0f) ? 1.0f / samps : 1.0f;
            }
        }
    }

    a->pendingChordMidiUpdate = true;
}

// ---------------------------------------------------------------------------
// Chord generator — randomises only the 8 degree values, resets trigger
// ---------------------------------------------------------------------------

static void generateProgression(ChordsAlg* a) {
    const int scaleIdx = a->v[kParamScale];

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

    // Generic pattern: T->SD->T->D->T->SD->D->T (always all 8 positions)
    static const uint8_t pat[8] = { 1, 0, 1, 2, 0, 1, 2, 0 };

    const int32_t algIdx = NT_algorithmIndex(a);
    const int     offset = (int)NT_parameterOffset();

    int firstDeg = -1;

    for (int pos = 0; pos < 8; ++pos) {
        const int func = pat[pos];

        // Build shuffled candidate list
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
            for (int d = 0; d < 7; ++d) {
                bool found = false;
                for (int j = 0; j < nCands; ++j) if (cands[j] == d) { found = true; break; }
                if (!found) cands[nCands++] = d;
            }
            for (int i = nPrim - 1; i > 0; --i) {
                int j = genLcgRange(a->rngState, i + 1);
                int tmp = cands[i]; cands[i] = cands[j]; cands[j] = tmp;
            }
        }

        int chosenDeg = cands[0];
        // Last chord must differ from first
        if (pos == 7 && chosenDeg == firstDeg && nCands > 1)
            chosenDeg = cands[1];

        if (pos == 0) firstDeg = chosenDeg;

        // Only set the degree parameter — do NOT touch ProgLength, Root, Inversion, etc.
        NT_setParameterFromAudio(algIdx, kParamDeg1 + pos * 2 + offset, (int16_t)chosenDeg);
    }

    // Reset the trigger only
    NT_setParameterFromAudio(algIdx, kParamGenerate + offset, 0);
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

    static const float kMin7Intervals[4] = { 0.0f, 3.0f, 7.0f, 10.0f };
    const float kC3Hz = kC0Hz * 8.0f;
    for (int i = 0; i < 4; ++i) {
        d->osc_freq[i]   = kC3Hz * powf(2.0f, kMin7Intervals[i] / 12.0f);
        d->osc_levelV[i] = 2.5f;
    }
    d->prevAdvTrig   = 0.0f;
    d->prevResetTrig = 0.0f;
    d->bassPhase          = 0.0f;
    d->bassFreq           = kC0Hz * 4.0f;
    d->bassTargetFreq     = d->bassFreq;
    d->bassGlideStartFreq = d->bassFreq;
    d->bassGlideProgress  = 1.0f;
    d->bassGlideDT        = 0.0f;
    d->bassLevelV         = 5.0f;
    d->prevClockTrig      = 0.0f;
    d->prevBassSample     = 0.0f;
    d->bass2Phase         = 0.0f;
    d->bass2LevelV        = 0.0f;
    d->bass2LevelSmoothed = 0.0f;
    d->prevBass2Sample    = 0.0f;
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
    d->fmDepthSmoothed    = 0.0f;
    d->melIndexSmoothed   = 0.0f;
    d->bassLevelSmoothed  = 5.0f;
    d->melLevelSmoothed   = 5.0f;

    ChordsAlg* a = new (ptrs.sram) ChordsAlg(d);
    a->parameters     = parameters;
    a->parameterPages = &paramPages;
    a->levelModDepth  = 0.0f;
    a->shapeModDepth  = 0.0f;
    for (int i = 0; i < 4; ++i)
        a->osc_gainF[i] = 1.0f + a->v[kParamOSC1Shape + i*2] * 8.0f / 1000.0f;
    a->melOSC2SemiPrev    = a->v[kParamMelOSC2Semi];
    a->melOSC2PartialPrev = a->v[kParamMelOSC2Partial];

    updateNotePool(a);
    updateMelNotePool(a);
    d->bassFreq           = a->notePool[0];
    d->bassTargetFreq     = a->notePool[0];
    d->bassGlideStartFreq = a->notePool[0];
    d->melFreq            = a->melNotePool[8];
    d->melTargetFreq      = d->melFreq;
    d->melGlideStartFreq  = d->melFreq;

    return a;
}

// ---------------------------------------------------------------------------
// parameterChanged
// ---------------------------------------------------------------------------

// Clamps at parameters[paramIdx].min/max (used for the octave shifts).
static inline void bumpParamClamped(ChordsAlg* a, int paramIdx, int delta) {
    const _NT_parameter& pi = parameters[paramIdx];
    int32_t v = (int32_t)a->v[paramIdx] + delta;
    if (v < pi.min) v = pi.min;
    if (v > pi.max) v = pi.max;
    NT_setParameterFromAudio(NT_algorithmIndex(a), paramIdx + NT_parameterOffset(), (int16_t)v);
}

// Wraps around at parameters[paramIdx].min/max (used for Root, a pitch class).
static inline void bumpParamWrapped(ChordsAlg* a, int paramIdx, int delta) {
    const _NT_parameter& pi = parameters[paramIdx];
    const int32_t range = pi.max - pi.min + 1;
    int32_t v = (int32_t)a->v[paramIdx] + delta - pi.min;
    v = ((v % range) + range) % range;
    v += pi.min;
    NT_setParameterFromAudio(NT_algorithmIndex(a), paramIdx + NT_parameterOffset(), (int16_t)v);
}

void parameterChanged(_NT_algorithm* self, int p) {
    ChordsAlg* a = (ChordsAlg*)self;
    if (p >= kParamInv1 && p <= kParamDeg8) {
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
        a->levelModDepth = a->v[kParamLevelMod] / 100.0f * 10.0f;
    } else if (p == kParamShapeMod) {
        a->shapeModDepth = a->v[kParamShapeMod] / 100.0f * 8.0f;
    } else if (p == kParamFMFreq) {
        a->fmFreqMult = (float)a->v[kParamFMFreq];
    } else if (p == kParamFMDepth) {
        a->fmDepth = a->v[kParamFMDepth] * 0.01f;
    } else if (p == kParamPhase) {
        a->phaseStep = a->v[kParamPhase] / 3600.0f;
    } else if (p == kParamScale) {
        updateFrequencies(a);
    } else if (p == kParamGenRoot) {
        updateFrequencies(a);
    } else if (p == kParamInversion) {
        updateFrequencies(a);
    } else if (p == kParamChordOctave) {
        updateFrequencies(a);
    } else if (p == kParamOctave) {
        // compatibility placeholder — no effect but harmless
        updateFrequencies(a);
    } else if (p == kParamBassLevel) {
        a->dtc->bassLevelV = a->v[kParamBassLevel] / 100.0f * 10.0f;
    } else if (p == kParamRandomise) {
        if (a->v[kParamRandomise] == 1) {
            for (int i = 0; i < 16; ++i) {
                a->melodyLoop[i]      = (float)(lcg_next() & 0xFFFFu) / 65535.0f;
                a->densityLoop[i]     = (float)(lcg_next() & 0xFFFFu) / 65535.0f;
                a->chordDensityLoop[i]= (float)(lcg_next() & 0xFFFFu) / 65535.0f;
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
    } else if (p == kParamBassSnap || p == kParamMelSnap) {
        updateFrequencies(a);
    } else if (p == kParamMelDetune) {
        const int partial = a->v[kParamMelOSC2Partial];
        const int semi    = a->v[kParamMelOSC2Semi];
        const int detune  = a->v[kParamMelDetune];
        a->melOSC2FreqScale = (float)partial *
            powf(2.0f, (float)semi / 12.0f + (float)detune / 1200.0f);
    } else if (p == kParamMelOSC2Partial) {
        // OSC2 Part and OSC2 Semi are both intended to be MIDI-mapped (in the
        // module's own mapping menu, with independently tuned min/max/delta)
        // to the same source — Ch5 CC21. Shift (Ch5 Note 62) decides which one
        // is currently authoritative: unshifted edits to Part are reverted so
        // only Semi responds; shift-held edits are accepted.
        if (!a->melShiftHeld && (int)a->v[kParamMelOSC2Partial] != a->melOSC2PartialPrev) {
            NT_setParameterFromAudio(NT_algorithmIndex(a), kParamMelOSC2Partial + NT_parameterOffset(),
                                      (int16_t)a->melOSC2PartialPrev);
        } else {
            a->melOSC2PartialPrev = a->v[kParamMelOSC2Partial];
        }
        const int partial = a->v[kParamMelOSC2Partial];
        const int semi    = a->v[kParamMelOSC2Semi];
        const int detune  = a->v[kParamMelDetune];
        a->melOSC2FreqScale = (float)partial *
            powf(2.0f, (float)semi / 12.0f + (float)detune / 1200.0f);
    } else if (p == kParamMelOSC2Semi) {
        // Same arbitration as OSC2 Part above, inverted: shift-held edits to
        // Semi are reverted so only Part responds; unshifted edits are accepted.
        if (a->melShiftHeld && (int)a->v[kParamMelOSC2Semi] != a->melOSC2SemiPrev) {
            NT_setParameterFromAudio(NT_algorithmIndex(a), kParamMelOSC2Semi + NT_parameterOffset(),
                                      (int16_t)a->melOSC2SemiPrev);
        } else {
            a->melOSC2SemiPrev = a->v[kParamMelOSC2Semi];
        }
        const int partial = a->v[kParamMelOSC2Partial];
        const int semi    = a->v[kParamMelOSC2Semi];
        const int detune  = a->v[kParamMelDetune];
        a->melOSC2FreqScale = (float)partial *
            powf(2.0f, (float)semi / 12.0f + (float)detune / 1200.0f);
    } else if (p == kParamMelIndex) {
        const float centsPerCopy = (float)a->v[kParamMelIndex] / 1000.0f * 50.0f;
        for (int i = 0; i < 7; ++i)
            a->melSwarmRatio[i] = powf(2.0f, (float)(i + 1) * centsPerCopy / 1200.0f);
    } else if (p == kParamBassOctave) {
        a->bassOctaveScale = powf(2.0f, (float)a->v[kParamBassOctave]);
    } else if (p == kParamBass2Level) {
        a->dtc->bass2LevelV = a->v[kParamBass2Level] / 100.0f * 10.0f;
    } else if (p == kParamBassDetune) {
        a->bass2FreqRatio = powf(2.0f, (float)a->v[kParamBassDetune] / 1200.0f);
    } else if (p == kParamAdvance) {
        if (a->v[kParamAdvance] == 1) {
            if (++a->advDivCount >= a->v[kParamProgDiv]) {
                a->advDivCount = 0;
                a->currentStep = (a->currentStep + 1) % a->v[kParamProgLength];
                updateFrequencies(a);
            }
            NT_setParameterFromAudio(NT_algorithmIndex(a), kParamAdvance + NT_parameterOffset(), 0);
        }
    } else if (p == kParamGenerate) {
        if (a->v[kParamGenerate] == 1)
            generateProgression(a);
    }
}

// ---------------------------------------------------------------------------
// draw() — progression overview with global inversion
// ---------------------------------------------------------------------------

bool draw(_NT_algorithm* self) {
    ChordsAlg* a         = (ChordsAlg*)self;
    const int progLength = a->v[kParamProgLength];
    const int activeStep = a->currentStep;
    const int globalInv  = a->v[kParamInversion];

    static const char* const romanNumerals[] = { "I", "II", "III", "IV", "V", "VI", "VII" };

    const int slotW   = 32;
    const int romanY  = 34;
    const int invY    = 47;
    const int frameY0 = 19;
    const int frameY1 = 50;

    const char invStr[2] = { (char)('0' + globalInv), 0 };

    for (int s = 0; s < 8; ++s) {
        if (s >= progLength) continue;

        const int deg = a->v[kParamDeg1 + s * 2];
        const int cx  = s * slotW + slotW / 2;
        const int col = (s == activeStep) ? 15 : 8;

        if (s == activeStep)
            NT_drawShapeI(kNT_box, s * slotW, frameY0, s * slotW + slotW - 1, frameY1, 8);

        NT_drawText(cx, romanY, romanNumerals[deg], col, kNT_textCentre, kNT_textNormal);
        NT_drawText(cx, invY, invStr, col, kNT_textCentre, kNT_textNormal);
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

    float* bassOut = busFrames + (a->v[kParamBassOutput] - 1) * numFrames;
    float* melOut  = busFrames + (a->v[kParamMelOutput]  - 1) * numFrames;

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

    const int melClockBus    = a->v[kParamMelClockIn];
    const float* melClockIn  = (melClockBus > 0) ? busFrames + (melClockBus - 1) * numFrames : nullptr;
    const int melLoopLength  = a->v[kParamMelLoopLength] > 0 ? a->v[kParamMelLoopLength] : 1;
    const int melRange       = a->v[kParamMelRange];
    const int melOffset      = a->v[kParamMelOffset];
    const int melDensity     = a->v[kParamMelDensity];
    const float melGlideSecs = a->v[kParamMelGlide] * 0.01f;
    const int melGlideCurve  = a->v[kParamMelGlideCurve];
    const int melNoteUpd     = a->v[kParamMelNoteUpdate];

    const int   melOSC1Wave  = a->v[kParamMelOSC1Wave];
    const int   melAlgo      = a->v[kParamMelAlgo];
    const float melOSC2Scale = a->melOSC2FreqScale;
    float swRat[7];
    for (int i = 0; i < 7; ++i) swRat[i] = a->melSwarmRatio[i];

    const float melLFODt    = (float)a->v[kParamMelLFOSpeed] * 0.01f / sr;
    const int   melLFOShape  = a->v[kParamMelLFOShape];
    const bool  melLFOReset  = (a->v[kParamMelLFOReset] != 0);
    const float melLFOAmt    = (float)a->v[kParamMelLFOAmt] * 0.01f;

    float prevTrig  = d->prevAdvTrig;
    float prevReset = d->prevResetTrig;
    const bool  hermite    = (a->v[kParamHermite] != 0);
    const float smoothCoeff     = 1.0f - expf(-1.0f / (0.005f * sr));
    float fmDepthSmoothed       = d->fmDepthSmoothed;
    float melIndexSmoothed      = d->melIndexSmoothed;
    float bassLvSm              = d->bassLevelSmoothed;
    float melLvSm               = d->melLevelSmoothed;
    const float bassTgt = d->bassLevelV;
    const float melTgt  = d->melLevelV;
    const float melIndexTarget  = (float)a->v[kParamMelIndex] / 1000.0f;

    float bass2Phase           = d->bass2Phase;
    const float bass2Tgt       = d->bass2LevelV;
    float bass2LvSm            = d->bass2LevelSmoothed;
    const float bass2FreqRatio = a->bass2FreqRatio;
    float prevBass2Sample      = d->prevBass2Sample;
    bool  bass2Gated           = a->bass2Gated;
    bool  bass2PendingGated    = a->bass2PendingGated;
    bool  bass2HasPending      = a->bass2HasPending;

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
    int   chordLoopPos       = a->chordLoopPos;
    bool  chordMidiGated     = a->chordMidiGated;
    int   bassNoteUpdateCount    = a->bassNoteUpdateCount;
    bool  bassHasPendingNote     = a->bassHasPendingNote;
    float bassNoteUpdateTargetHz = a->bassNoteUpdateTargetHz;

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

    // MIDI block-constant values
    const int      bassCH        = a->v[kParamBassCH];
    const int      melCH         = a->v[kParamMelCH];
    const int      chordCHs[4]   = { a->v[kParamChord1CH], a->v[kParamChord2CH],
                                      a->v[kParamChord3CH], a->v[kParamChord4CH] };
    const int      chordDensityP = a->v[kParamChordDensity];
    const uint32_t midiDest      = a->v[kParamMIDIOut] ? kNT_destinationUSB : kNT_destinationBreakout;

    // Chord MIDI: chord pitch changed (progression advanced) — update notes while respecting gate
    if (a->pendingChordMidiUpdate) {
        a->pendingChordMidiUpdate = false;
        if (chordMidiGated) {
            // NoteOff for old notes, NoteOn for new notes
            for (int v = 0; v < 4; ++v) {
                if (chordCHs[v] > 0 && a->chordMidiNotes[v] >= 0)
                    NT_sendMidi3ByteMessage(midiDest, 0x80 | (uint8_t)(chordCHs[v] - 1),
                                            (uint8_t)a->chordMidiNotes[v], 0);
            }
            for (int v = 0; v < 4; ++v) {
                const int note = hzToMidi(d->osc_freq[v]);
                a->chordMidiNotes[v] = note;
                if (chordCHs[v] > 0)
                    NT_sendMidi3ByteMessage(midiDest, 0x90 | (uint8_t)(chordCHs[v] - 1),
                                            (uint8_t)note, 100);
            }
        } else {
            // Not gated — just refresh stored note values silently
            for (int v = 0; v < 4; ++v)
                a->chordMidiNotes[v] = hzToMidi(d->osc_freq[v]);
        }
    }

    for (int i = 0; i < numFrames; ++i) {
        if (rst) {
            const float r = rst[i];
            if (prevReset < 1.0f && r >= 1.0f) {
                a->currentStep = 0;
                updateFrequencies(a);
            }
            prevReset = r;
        }

        if (adv) {
            const float t = adv[i];
            if (prevTrig < 1.0f && t >= 1.0f) {
                if (++a->advDivCount >= progDiv) {
                    a->advDivCount = 0;
                    a->currentStep = (a->currentStep + 1) % progLength;
                    updateFrequencies(a);
                }
            }
            prevTrig = t;
        }

        fmDepthSmoothed  += (a->fmDepth        - fmDepthSmoothed)  * smoothCoeff;
        melIndexSmoothed += (melIndexTarget     - melIndexSmoothed) * smoothCoeff;
        bassLvSm  += (bassTgt  - bassLvSm)  * smoothCoeff;
        bass2LvSm += (bass2Tgt - bass2LvSm) * smoothCoeff;
        melLvSm   += (melTgt   - melLvSm)   * smoothCoeff;

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
                if (newGate != bassGated)  { bassPendingGated  = newGate; bassHasPending  = true; }
                if (newGate != bass2Gated) { bass2PendingGated = newGate; bass2HasPending = true; }

                // Chord rhythm (same clock as bass)
                chordLoopPos = (chordLoopPos + 1) % loopLength;
                const bool newChordGate = (a->chordDensityLoop[chordLoopPos] < (float)chordDensityP / 100.0f);
                if (newChordGate != chordMidiGated) {
                    chordMidiGated = newChordGate;
                    for (int v = 0; v < 4; ++v) {
                        if (chordCHs[v] > 0 && a->chordMidiNotes[v] >= 0) {
                            const uint8_t st = chordMidiGated
                                ? (0x90 | (uint8_t)(chordCHs[v] - 1))
                                : (0x80 | (uint8_t)(chordCHs[v] - 1));
                            NT_sendMidi3ByteMessage(midiDest, st, (uint8_t)a->chordMidiNotes[v], chordMidiGated ? 100 : 0);
                        }
                    }
                }
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
        float bassSample = (bassWave == 0)
            ? sinf(2.0f * kPi * bassPhase)
            : melOscWave(bassWave, bassPhase, bassDt, hermite);

        if ((bassHasPending || bassHasPendingNote) && prevBassSample * bassSample <= 0.0f) {
            if (bassHasPending) {
                const bool prevBassGated = bassGated;
                if (!bassGated && bassPendingGated) bassNoteUpdateCount = 0;
                bassGated = bassPendingGated; bassHasPending = false;
                if (prevBassGated != bassGated && bassCH > 0) {
                    const uint8_t mCh = (uint8_t)(bassCH - 1);
                    if (bassGated) {
                        const int note = hzToMidi(bassFreq * a->bassOctaveScale);
                        if (a->bassMidiNote >= 0) NT_sendMidi3ByteMessage(midiDest, 0x80 | mCh, (uint8_t)a->bassMidiNote, 0);
                        a->bassMidiNote = note;
                        NT_sendMidi3ByteMessage(midiDest, 0x90 | mCh, (uint8_t)note, 100);
                    } else {
                        if (a->bassMidiNote >= 0) NT_sendMidi3ByteMessage(midiDest, 0x80 | mCh, (uint8_t)a->bassMidiNote, 0);
                        a->bassMidiNote = -1;
                    }
                }
            }
            if (bassHasPendingNote) {
                if (bassGated && bassCH > 0) {
                    const int newNote = hzToMidi(bassNoteUpdateTargetHz * a->bassOctaveScale);
                    const uint8_t mCh = (uint8_t)(bassCH - 1);
                    if (a->bassMidiNote >= 0) NT_sendMidi3ByteMessage(midiDest, 0x80 | mCh, (uint8_t)a->bassMidiNote, 0);
                    a->bassMidiNote = newNote;
                    NT_sendMidi3ByteMessage(midiDest, 0x90 | mCh, (uint8_t)newNote, 100);
                }
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
        const float bass2Dt = bassFreq * bass2FreqRatio / sr;
        float bass2Sample = (bassWave == 0)
            ? sinf(2.0f * kPi * bass2Phase)
            : melOscWave(bassWave, bass2Phase, bass2Dt, hermite);
        if (bass2HasPending && prevBass2Sample * bass2Sample <= 0.0f) {
            bass2Gated = bass2PendingGated;
            bass2HasPending = false;
        }
        prevBass2Sample = bass2Sample;
        bassOut[i] += (bassGated  ? bassSample  * bassLvSm  : 0.0f)
                    + (bass2Gated ? bass2Sample * bass2LvSm : 0.0f);
        bassPhase  += bassFreq * a->bassOctaveScale / sr;
        if (bassPhase  >= 1.0f) bassPhase  -= 1.0f;
        bass2Phase += bassFreq * a->bassOctaveScale * bass2FreqRatio / sr;
        if (bass2Phase >= 1.0f) bass2Phase -= 1.0f;
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

        if (melHasPending) {
            const bool wasGated = melGated;
            if (!melGated && melPendingGated) melNoteUpdateCount = 0;
            melGated = melPendingGated;
            melHasPending = false;
            if (!wasGated && melGated && melLFOReset)
                melLFOPhase = 0.0f;
            if (wasGated != melGated && melCH > 0) {
                const uint8_t mCh = (uint8_t)(melCH - 1);
                if (melGated) {
                    const int note = hzToMidi(melFreq);
                    if (a->melMidiNote >= 0) NT_sendMidi3ByteMessage(midiDest, 0x80 | mCh, (uint8_t)a->melMidiNote, 0);
                    a->melMidiNote = note;
                    NT_sendMidi3ByteMessage(midiDest, 0x90 | mCh, (uint8_t)note, 100);
                } else {
                    if (a->melMidiNote >= 0) NT_sendMidi3ByteMessage(midiDest, 0x80 | mCh, (uint8_t)a->melMidiNote, 0);
                    a->melMidiNote = -1;
                }
            }
        }
        if (melHasPendingNote && prevMelSample * melCoreSample <= 0.0f) {
            if (melGated && melCH > 0) {
                const int newNote = hzToMidi(melNoteUpdateTargetHz);
                const uint8_t mCh = (uint8_t)(melCH - 1);
                if (a->melMidiNote >= 0) NT_sendMidi3ByteMessage(midiDest, 0x80 | mCh, (uint8_t)a->melMidiNote, 0);
                a->melMidiNote = newNote;
                NT_sendMidi3ByteMessage(midiDest, 0x90 | mCh, (uint8_t)newNote, 100);
            }
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

        if (melGated) {
            melFadeGain += (1.0f / 64.0f); if (melFadeGain > 1.0f) melFadeGain = 1.0f;
        } else {
            melFadeGain -= (1.0f / 64.0f); if (melFadeGain < 0.0f) melFadeGain = 0.0f;
        }

        const float melLFOWave = posWave1(melLFOShape, melLFOPhase);
        float effectiveIndexF = melIndexSmoothed + melLFOAmt * melLFOWave;
        if (effectiveIndexF > 1.0f) effectiveIndexF = 1.0f;
        melLFOPhase += melLFODt;
        if (melLFOPhase >= 1.0f) melLFOPhase -= 1.0f;

        float melSample;
        float melPhaseAdv = melDt;

        if (melFadeGain > 0.0f) {
            if (melAlgo == 2) {
                float sum = melCoreSample;
                for (int ci = 0; ci < 7; ++ci) {
                    const float swDt = melFreq * swRat[ci] / sr;
                    sum += melOscWave(melOSC1Wave, swPh[ci], swDt, hermite);
                    swPh[ci] += swDt;
                    swPh[ci] -= floorf(swPh[ci]);
                }
                melSample = sum * 0.353553f;
            } else {
                const float dt2  = melFreq * melOSC2Scale / sr;
                const float osc2 = (dt2 > 0.0f) ? (hermite ? wtReadH(g_wt, mel2Phase) : wtRead(g_wt, mel2Phase)) : 0.0f;
                mel2Phase += dt2;
                mel2Phase -= floorf(mel2Phase);

                switch (melAlgo) {
                case 0: {
                    const float g1 = cosf(kPi * 0.5f * effectiveIndexF);
                    const float g2 = sinf(kPi * 0.5f * effectiveIndexF);
                    melSample = melCoreSample * g1 + osc2 * g2;
                    break;
                }
                case 1: {
                    const float fmDt = melDt * (1.0f + osc2 * effectiveIndexF * 10.0f);
                    melSample = melOscWave(melOSC1Wave, melPhase, fmDt, hermite);
                    melPhaseAdv = fmDt;
                    break;
                }
                case 3: {
                    const float pmOff = osc2 * effectiveIndexF * 5.0f;
                    float pmPh = melPhase + pmOff;
                    pmPh -= floorf(pmPh);
                    melSample = melOscWave(melOSC1Wave, pmPh, melDt, hermite);
                    break;
                }
                default:
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
        // No chord audio voice — the chord is now only the Chord1-4 CH MIDI
        // stream (sent from d->osc_freq[] below / on pendingChordMidiUpdate).
    }

    // Write-back
    d->prevAdvTrig   = prevTrig;
    d->prevResetTrig = prevReset;
    d->bassPhase=bassPhase; d->bassFreq=bassFreq; d->bassTargetFreq=bassTargetFreq;
    d->bassGlideStartFreq=bassGlideStartFreq; d->bassGlideProgress=bassGlideProgress;
    d->bassGlideDT=bassGlideDT; d->prevClockTrig=prevClockTrig; d->prevBassSample=prevBassSample;
    d->bass2Phase=bass2Phase; d->bass2LevelSmoothed=bass2LvSm;
    d->prevBass2Sample=prevBass2Sample;
    a->bass2Gated=bass2Gated; a->bass2PendingGated=bass2PendingGated; a->bass2HasPending=bass2HasPending;
    a->bassGated=bassGated; a->bassPendingGated=bassPendingGated; a->bassHasPending=bassHasPending;
    a->bassLoopPos=bassLoopPos; a->bassNoteUpdateCount=bassNoteUpdateCount;
    a->chordLoopPos=chordLoopPos; a->chordMidiGated=chordMidiGated;
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
    d->bassLevelSmoothed = bassLvSm;
    d->melLevelSmoothed  = melLvSm;
}

// ---------------------------------------------------------------------------
// midiMessage() — dedicated MIDI notes nudge octave/root parameters
// ---------------------------------------------------------------------------

void midiMessage(_NT_algorithm* self, uint8_t byte0, uint8_t byte1, uint8_t byte2) {
    ChordsAlg* a = (ChordsAlg*)self;
    const int status  = byte0 & 0xF0;
    const int channel = (byte0 & 0x0F) + 1;      // 1-based (1-16)
    const int note     = byte1;

    // Melody-page shift button (Ch5 Note 62): tracked across Note On/Off so
    // holding it can redirect other edits (see kParamMelOSC2Semi handling
    // in parameterChanged()). Consumes both Note On and Note Off for note 62.
    if (channel == 5 && note == 62) {
        if (status == 0x90 && byte2 > 0)       a->melShiftHeld = true;
        else if (status == 0x80 || status == 0x90) a->melShiftHeld = false; // Note Off, or Note On vel 0
        return;
    }

    if (status != 0x90 || byte2 == 0) return;   // Note On, velocity > 0 only

    switch (channel) {
    case 1:  // Bass octave / range / offset
        if      (note == 66) bumpParamClamped(a, kParamBassOctave, -1);
        else if (note == 67) bumpParamClamped(a, kParamBassOctave, +1);
        else if (note == 64) bumpParamClamped(a, kParamBassRange, -1);
        else if (note == 63) bumpParamClamped(a, kParamBassRange, +1);
        else if (note == 62) bumpParamWrapped(a, kParamBassOffset, +1);
        break;
    case 4:  // Chord (4-voice) octave
        if      (note == 66) bumpParamClamped(a, kParamChordOctave, -1);
        else if (note == 67) bumpParamClamped(a, kParamChordOctave, +1);
        break;
    case 5:  // Melody octave / range / offset / wave / algo
        if      (note == 66) bumpParamClamped(a, kParamMelOctave, -1);
        else if (note == 67) bumpParamClamped(a, kParamMelOctave, +1);
        else if (note == 64) bumpParamClamped(a, kParamMelRange, -1);
        else if (note == 63) bumpParamClamped(a, kParamMelRange, +1);
        else if (note == 65) bumpParamWrapped(a, kParamMelOffset, +1);
        else if (note == 68) bumpParamWrapped(a, kParamMelOSC1Wave, +1);
        else if (note == 61) bumpParamWrapped(a, kParamMelAlgo, +1);
        break;
    case 13: // Generator root (wraps — it's a pitch class)
        if      (note == 85) bumpParamWrapped(a, kParamGenRoot, -1);
        else if (note == 86) bumpParamWrapped(a, kParamGenRoot, +1);
        break;
    }
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
    .midiMessage                 = midiMessage,
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
