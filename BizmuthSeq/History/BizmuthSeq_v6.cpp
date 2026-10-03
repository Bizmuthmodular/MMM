#include "distingnt/api.h"
#include <new>
#include <cstdlib>  // rand
#include <cstring>  // memset

#define MAX_SEQS   16
#define MAX_STEPS  16
#define TRIG_SEQS   8
#define TRIG_STEPS 16
#define SEQ_TRACKS  8
#define NUM_PRESETS 16

// ----- Parameter indices -----
enum {
    // Original CC generator params (0..84)
    IDX_RANDOMIZE    = 0,
    IDX_RANDOMNESS,           // 1
    IDX_FREEZ,                // 2 - NEW: global CC freeze/sample-hold
    IDX_INCLUDE_BASE,         // 3..18
    IDX_MIDI_OUT      = 19,
    IDX_CLOCK_INPUT,
    IDX_TRIGGER_MIDI,         // 21 - hidden parameter for immediate MIDI triggering
    IDX_PARAM_BASE    = 22,   // 22..85 (16*4)
    CC_NUM_PARAMS     = IDX_PARAM_BASE + (MAX_SEQS*4),

    // Original Trigger generator params
    IDX_TRIG_RND      = CC_NUM_PARAMS,       // 86
    IDX_TRIG_THRESH,                         // 87
    IDX_TRIG_LEN_BASE,                       // 88..95

    // NEW: Step Sequencer params (96+)
    IDX_SEQ_BASE      = IDX_TRIG_LEN_BASE + TRIG_SEQS,  // 96
    IDX_SEQ_PRESET,                                     // 97 - current preset
    IDX_SEQ_TARGET,                                     // 98 - target preset
    IDX_SEQ_CROSSFADE,                                  // 99 - crossfade amount
    IDX_SEQ_START_STOP,                                 // 100 - start/stop control
    IDX_SEQ_TRACK_LENS,                                 // 101..106 - track lengths (1-6)

    // NEW: Sub-step Offset params (107+) - timing offsets in sub-steps (-8 to +8)
    IDX_OFFSET_BASE   = IDX_SEQ_TRACK_LENS + 6,        // 107..112 - sub-step offsets for tracks 1-6
    IDX_OFFSETS_ON    = IDX_OFFSET_BASE + 6,           // 113 - enable/disable offsets
    IDX_RND_OFFSETS   = IDX_OFFSETS_ON + 1,            // 114 - randomize offsets (0->1 trigger)
    IDX_RND_OFF_AMT   = IDX_RND_OFFSETS + 1,           // 115 - randomization amount (0-8)
    IDX_BURST_RATE    = IDX_RND_OFF_AMT + 1,           // 116 - Burst rate (1-15 Hz)
    IDX_BURST_COUNT   = IDX_BURST_RATE + 1,            // 117 - Burst count (0-8)
    IDX_BURST_RND     = IDX_BURST_COUNT + 1,           // 118 - Burst randomization (ON/OFF)
    IDX_BURST_RND_AMT = IDX_BURST_RND + 1,             // 119 - Burst random amount (0-100%)

    // Delta LFO params (120-138)
    IDX_DELTA_BASE    = IDX_BURST_RND_AMT + 1,         // 120-135 - Delta enable for tracks 1-16
    IDX_DELTA_AMT     = IDX_DELTA_BASE + 16,           // 136 - Delta amount (-127 to +127)
    IDX_DELTA_SCOPE   = IDX_DELTA_AMT + 1,             // 137 - Delta scope (2-64 repetitions)
    IDX_DELTA_SHAPE   = IDX_DELTA_SCOPE + 1,           // 138 - Delta shape (Ramp/Saw/Triangle/Square)
    IDX_DRM_LENGTHS   = IDX_DELTA_SHAPE + 1,           // 139 - Drum length preset (1-8)
    IDX_MORPH_X       = IDX_DRM_LENGTHS + 1,           // 140 - Morph X coordinate (0-127)
    IDX_MORPH_Y       = IDX_MORPH_X + 1,               // 141 - Morph Y coordinate (0-127)
    IDX_ZERO          = IDX_MORPH_Y + 1,               // 142 - Zero trigger output (0→1 creates silent holes)

    // NOTE: Step data controlled via MIDI, not parameters

    NUM_PARAMS       = IDX_ZERO + 1  // = 143 parameters (0-142)
};

// ----- UI enums -----
static const char* enumOnOff[]   = { "OFF", "ON", nullptr };
static const char* enumUsbDin[]  = { "USB", "DIN", nullptr };
static const char* dirEnums[]    = { "FWD", "BWD", "RND", nullptr };
static const char* deltaShapeEnums[] = { "Ramp", "Saw", "Triangle", "Square", nullptr };

// ----- Drum Length Presets -----
// 8 presets, each with 6 track lengths (tracks 1-6)
// Order: Kick, Sub, OH, Ride, Snare, CH
static const uint8_t DRUM_LENGTH_PRESETS[8][6] = {
    {16, 16, 16, 16, 16, 16},  // Preset 1: All 16
    {12, 12,  4,  6, 10,  8},  // Preset 2
    {10, 10,  6,  4,  6,  8},  // Preset 3
    {14, 14, 10,  8,  6,  4},  // Preset 4: Kick/Snare break 4/4 grid
    {14, 14, 12,  8, 10,  6},  // Preset 5: Kick/Snare break 4/4 grid, longer pattern
    { 7,  7, 14,  8,  6, 12},  // Preset 6: Odd Kick/Sub, Snare breaks 4/4 grid
    { 7,  7,  9,  9,  6,  5},  // Preset 7: Odd numbers
    {14, 14, 12,  4,  7,  8}   // Preset 8: Kick/Snare break 4/4 grid
};

// ----- Sequence Presets -----
// Tracks 1-6 arranged as 4x4 grids for easy manual editing
// Tracks 7-8 handled separately (not part of preset system)
static const uint8_t SEQUENCE_PRESETS[NUM_PRESETS][6][16] = {
    // Preset 1
    {
        // Track 1: Kick
        {1,0,0,0,
         1,0,0,0,
         1,0,0,0,
         1,0,0,0},
        // Track 2: Sub
        {0,0,1,0,
         0,1,0,0,
         0,0,0,1,
         0,1,0,0},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         1,0,0,0,
         0,0,0,0,
         1,0,0,0},
        // Track 6: CH
        {1,1,1,1,
         1,1,1,1,
         1,1,1,1,
         1,1,1,1}
    },

    // Preset 2
    {
        // Track 1: Kick
        {1,0,0,0,
         1,0,0,0,
         1,0,0,0,
         1,0,0,0},
        // Track 2: Sub
        {0,1,0,1,
         0,0,1,0,
         0,1,0,1,
         0,0,1,0},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         0,0,0,0,
         1,0,0,0,
         0,0,0,0},
        // Track 6: CH
        {0,0,1,1,
         0,0,1,1,
         0,0,1,1,
         0,0,1,1}
    },

    // Preset 3
    {
        // Track 1: Kick
        {1,0,0,0,
         1,0,0,0,
         1,0,0,0,
         1,0,0,0},
        // Track 2: Sub
        {0,0,1,0,
         0,1,0,1,
         0,0,1,0,
         0,1,0,1},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         1,0,0,0,
         0,0,0,0,
         1,0,0,0},
        // Track 6: CH
        {1,1,1,1,
         1,1,1,1,
         1,1,1,1,
         1,1,1,1}
    },

    // Preset 4
    {
        // Track 1: Kick
        {1,0,0,0,
         1,0,0,0,
         1,0,0,0,
         1,0,0,0},
        // Track 2: Sub
        {0,0,0,1,
         0,0,1,0,
         0,0,0,1,
         0,0,1,1},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         1,0,0,0,
         0,0,0,0,
         1,0,0,0},
        // Track 6: CH
        {0,1,1,1,
         0,1,1,1,
         0,1,1,1,
         0,1,1,1}
    },

    // Preset 5
    {
        // Track 1: Kick
        {1,0,0,1,
         0,0,1,0,
         0,0,1,0,
         0,0,1,1},
        // Track 2: Sub
        {0,1,0,0,
         0,0,0,0,
         0,0,0,1,
         0,0,1,0},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         0,0,0,0,
         1,0,0,0,
         0,0,0,0},
        // Track 6: CH
        {1,1,1,1,
         1,1,1,1,
         1,1,0,1,
         1,1,0,1}
    },

    // Preset 6
    {
        // Track 1: Kick
        {1,0,0,0,
         1,0,0,0,
         1,0,0,0,
         1,0,0,0},
        // Track 2: Sub
        {0,1,1,0,
         0,0,1,1,
         0,1,1,0,
         0,1,0,1},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         1,0,0,0,
         0,0,0,0,
         1,0,0,0},
        // Track 6: CH
        {1,1,1,1,
         0,1,1,1,
         1,1,1,1,
         0,1,1,1}
    },

    // Preset 7
    {
        // Track 1: Kick
        {1,0,0,0,
         1,0,0,0,
         1,0,0,0,
         1,0,0,1},
        // Track 2: Sub
        {0,0,1,0,
         0,1,0,0,
         0,0,1,1,
         0,0,1,0},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         1,0,0,0,
         0,0,0,0,
         1,0,0,0},
        // Track 6: CH
        {0,0,1,1,
         1,1,1,1,
         0,0,1,1,
         1,1,1,1}
    },

    // Preset 8
    {
        // Track 1: Kick
        {1,0,0,0,
         1,0,0,0,
         1,0,0,0,
         1,0,0,1},
        // Track 2: Sub
        {0,1,0,1,
         0,0,0,1,
         0,0,0,1,
         0,0,1,1},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         1,0,0,0,
         0,0,0,0,
         1,0,0,1},
        // Track 6: CH
        {1,1,1,0,
         0,1,1,1,
         1,1,1,0,
         0,1,1,1}
    },

    // Preset 9
    {
        // Track 1: Kick
        {1,0,0,1,
         0,0,1,0,
         0,0,0,1,
         0,1,1,0},
        // Track 2: Sub
        {0,0,1,0,
         0,1,0,0,
         0,0,1,0,
         0,1,0,1},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         0,0,0,0,
         1,0,0,0,
         0,0,0,0},
        // Track 6: CH
        {1,1,1,1,
         1,1,1,1,
         1,1,1,1,
         1,1,1,1}
    },

    // Preset 10
    {
        // Track 1: Kick
        {1,0,0,1,
         0,0,1,0,
         1,0,0,1,
         1,1,0,1},
        // Track 2: Sub
        {0,1,0,0,
         0,0,0,1,
         0,1,1,0,
         0,0,1,1},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         1,0,0,0,
         0,0,0,0,
         1,0,0,0},
        // Track 6: CH
        {0,1,1,1,
         0,1,1,1,
         0,1,1,1,
         0,1,1,1}
    },

    // Preset 11
    {
        // Track 1: Kick
        {1,0,0,1,
         0,0,1,0,
         0,0,1,0,
         0,1,1,1},
        // Track 2: Sub
        {0,0,1,0,
         0,0,0,1,
         0,1,0,0,
         0,1,0,1},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         0,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         0,0,0,0,
         1,0,0,0,
         0,0,0,0},
        // Track 6: CH
        {1,1,1,1,
         1,1,1,1,
         1,1,1,1,
         1,1,1,1}
    },

    // Preset 12
    {
        // Track 1: Kick
        {1,0,1,1,
         0,0,1,0,
         0,1,1,0,
         0,0,1,1},
        // Track 2: Sub
        {0,1,0,0,
         0,1,0,0,
         0,1,0,1,
         0,1,0,0},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,1},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         1,0,0,0,
         0,0,0,0,
         1,0,0,0},
        // Track 6: CH
        {1,0,1,0,
         1,1,1,1,
         1,1,1,0,
         0,1,0,1}
    },

    // Preset 13
    {
        // Track 1: Kick
        {1,0,0,1,
         0,0,1,0,
         0,0,0,1,
         0,1,1,0},
        // Track 2: Sub
        {0,1,0,0,
         0,1,0,0,
         1,1,0,1,
         0,0,0,0},
        // Track 3: OH
        {0,0,1,0,
         0,0,0,1,
         0,0,1,0,
         0,0,0,1},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,1,
         1,0,1,0,
         1,0,1,1},
        // Track 5: Snare
        {0,0,0,0,
         0,0,0,0,
         1,0,0,0,
         0,0,0,0},
        // Track 6: CH
        {1,0,1,0,
         1,0,1,1,
         1,1,1,1,
         1,0,1,0}
    },

    // Preset 14
    {
        // Track 1: Kick
        {1,0,1,0,
         0,0,0,0,
         0,0,0,1,
         0,0,1,1},
        // Track 2: Sub
        {0,0,0,0,
         1,1,1,1,
         0,0,0,0,
         1,1,0,0},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         1,0,0,0,
         0,0,1,0,
         0,0,0,1},
        // Track 6: CH
        {1,1,1,1,
         1,1,1,1,
         1,1,1,1,
         1,1,1,1}
    },

    // Preset 15
    {
        // Track 1: Kick
        {1,0,1,0,
         0,0,1,0,
         0,0,0,0,
         0,1,1,1},
        // Track 2: Sub
        {0,0,0,0,
         0,1,0,0,
         1,1,1,1,
         0,0,0,0},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         0,0,0,0,
         1,0,0,0,
         0,0,1,0},
        // Track 6: CH
        {1,1,1,0,
         1,1,1,1,
         1,1,1,0,
         1,1,1,1}
    },

    // Preset 16
    {
        // Track 1: Kick
        {1,0,1,0,
         0,0,1,0,
         0,0,0,1,
         0,1,0,1},
        // Track 2: Sub
        {0,1,0,0,
         0,0,0,1,
         1,0,1,0,
         1,0,0,0},
        // Track 3: OH
        {0,0,1,0,
         0,0,1,0,
         0,0,1,0,
         0,0,1,0},
        // Track 4: Ride
        {1,0,1,0,
         1,0,1,0,
         1,0,1,0,
         1,0,1,0},
        // Track 5: Snare
        {0,0,0,0,
         1,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 6: CH
        {1,1,1,1,
         1,1,1,1,
         1,1,1,1,
         1,1,1,0}
    }
};

// ----- Parameter storage (EXPERT SLEEPERS PATTERN) -----
static const _NT_parameter parameters[] = {
    // Original CC params - YOUR DESIRED DEFAULTS
    { .name = "Randomise!", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },            // IDX_RANDOMIZE = 0
    { .name = "RandomNESS", .min = 0, .max = 127, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_RANDOMNESS = 1
    { .name = "FREEZ!", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },              // IDX_FREEZ = 2 - NEW: Global CC freeze

    // Include parameters (3-18)
    { .name = "Include 1", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },           // IDX_INCLUDE_BASE + 0
    { .name = "Include 2", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },           // IDX_INCLUDE_BASE + 1
    { .name = "Include 3", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },           // IDX_INCLUDE_BASE + 2
    { .name = "Include 4", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },           // IDX_INCLUDE_BASE + 3
    { .name = "Include 5", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },           // IDX_INCLUDE_BASE + 4
    { .name = "Include 6", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },           // IDX_INCLUDE_BASE + 5
    { .name = "Include 7", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },           // IDX_INCLUDE_BASE + 6
    { .name = "Include 8", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },           // IDX_INCLUDE_BASE + 7
    { .name = "Include 9", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },           // IDX_INCLUDE_BASE + 8
    { .name = "Include 10", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },          // IDX_INCLUDE_BASE + 9
    { .name = "Include 11", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },          // IDX_INCLUDE_BASE + 10
    { .name = "Include 12", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },          // IDX_INCLUDE_BASE + 11
    { .name = "Include 13", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },          // IDX_INCLUDE_BASE + 12
    { .name = "Include 14", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },          // IDX_INCLUDE_BASE + 13
    { .name = "Include 15", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },          // IDX_INCLUDE_BASE + 14
    { .name = "Include 16", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },          // IDX_INCLUDE_BASE + 15

    { .name = "MIDI Out", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumUsbDin },           // IDX_MIDI_OUT = 19 - DEFAULT: DIN
    { .name = "Clock input", .min = 1, .max = 28, .def = 21, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },        // IDX_CLOCK_INPUT = 20 - YOUR DEFAULT: 21
    { .name = "MIDI Trigger", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },     // IDX_TRIGGER_MIDI = 21

    // CC Parameters (21-84) - Steps, Div, Range, Dir for each of 16 sequences - YOUR DESIRED DEFAULTS
    { .name = "Steps 1", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },             // IDX_PARAM_BASE + 0*4 + 0
    { .name = "Div 1", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },               // IDX_PARAM_BASE + 0*4 + 1
    { .name = "Range 1", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_PARAM_BASE + 0*4 + 2
    { .name = "Dir 1", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },               // IDX_PARAM_BASE + 0*4 + 3

    { .name = "Steps 2", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },             // IDX_PARAM_BASE + 1*4 + 0
    { .name = "Div 2", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },               // IDX_PARAM_BASE + 1*4 + 1
    { .name = "Range 2", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_PARAM_BASE + 1*4 + 2
    { .name = "Dir 2", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },               // IDX_PARAM_BASE + 1*4 + 3

    { .name = "Steps 3", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 3", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 3", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 3", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 4", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 4", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 4", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 4", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 5", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 5", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 5", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 5", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 6", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 6", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 6", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 6", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 7", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 7", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 7", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 7", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 8", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 8", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 8", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 8", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 9", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 9", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 9", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 9", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 10", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 10", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 10", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 10", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 11", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 11", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 11", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 11", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 12", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 12", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 12", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 12", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 13", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 13", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 13", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 13", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 14", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 14", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 14", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 14", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 15", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 15", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 15", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 15", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    { .name = "Steps 16", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Div 16", .min = 1, .max = 32, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 16", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Dir 16", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = dirEnums },

    // Original trigger params (85-94)
    { .name = "Triggers RND", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },         // IDX_TRIG_RND = 85
    { .name = "Threshold", .min = 1, .max = 128, .def = 90, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_TRIG_THRESH = 86 - YOUR DEFAULT: 90

    { .name = "Length 1", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 0
    { .name = "Length 2", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 1
    { .name = "Length 3", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 2
    { .name = "Length 4", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 3
    { .name = "Length 5", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 4
    { .name = "Length 6", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 5
    { .name = "Length 7", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 6
    { .name = "Length 8", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 7

    // NEW: Step Sequencer params (95-105) - FIXED INDEX ALIGNMENT
    { .name = "SeqBase", .min = 0, .max = 0, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },              // IDX_SEQ_BASE = 95 (unused marker)
    { .name = "Preset", .min = 1, .max = 16, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },               // IDX_SEQ_PRESET = 96
    { .name = "Target", .min = 1, .max = 16, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },               // IDX_SEQ_TARGET = 97
    { .name = "Crossfade", .min = 0, .max = 127, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_SEQ_CROSSFADE = 98
    { .name = "Start/Stop", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },         // IDX_SEQ_START_STOP = 99

    { .name = "Seq Len 1", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_SEQ_TRACK_LENS + 0
    { .name = "Seq Len 2", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_SEQ_TRACK_LENS + 1
    { .name = "Seq Len 3", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_SEQ_TRACK_LENS + 2
    { .name = "Seq Len 4", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_SEQ_TRACK_LENS + 3
    { .name = "Seq Len 5", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_SEQ_TRACK_LENS + 4
    { .name = "Seq Len 6", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_SEQ_TRACK_LENS + 5 = 106

    // NEW: Sub-step offset parameters (-8 to +8 sub-steps)
    { .name = "KickOFF", .min = -8, .max = 8, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },           // IDX_OFFSET_BASE + 0 = 107 - Track 1: Kick
    { .name = "SubOFF", .min = -8, .max = 8, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_OFFSET_BASE + 1 = 108 - Track 2: Sub
    { .name = "OHOFF", .min = -8, .max = 8, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },             // IDX_OFFSET_BASE + 2 = 109 - Track 3: OH
    { .name = "RideOFF", .min = -8, .max = 8, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },           // IDX_OFFSET_BASE + 3 = 110 - Track 4: Ride
    { .name = "SnareOFF", .min = -8, .max = 8, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_OFFSET_BASE + 4 = 111 - Track 5: Snare
    { .name = "CHOFF", .min = -8, .max = 8, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },             // IDX_OFFSET_BASE + 5 = 112 - Track 6: CH

    // NEW: Offset control parameters
    { .name = "OffsetsON", .min = 0, .max = 1, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },        // IDX_OFFSETS_ON = 113 - Enable/disable offsets
    { .name = "RNDOffsets", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },         // IDX_RND_OFFSETS = 114 - Randomize trigger (0->1)
    { .name = "RNDoffAMT", .min = 0, .max = 8, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_RND_OFF_AMT = 115 - Random offset amount (0-8)
    { .name = "BurstRate", .min = 10, .max = 200, .def = 100, .unit = kNT_unitHz, .scaling = kNT_scaling10, .enumStrings = nullptr },          // IDX_BURST_RATE = 116 - Burst rate (1.0-20.0 Hz, 0.1 Hz resolution)
    { .name = "BCount", .min = 0, .max = 8, .def = 4, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },             // IDX_BURST_COUNT = 117 - Burst count (0-8)
    { .name = "BSpRand", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },          // IDX_BURST_RND = 118 - Burst randomization (ON/OFF)
    { .name = "BrndAMT", .min = 0, .max = 100, .def = 50, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = nullptr },      // IDX_BURST_RND_AMT = 119 - Burst random amount (0-100%)

    // Delta LFO parameters (120-138)
    { .name = "Delta1", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },            // IDX_DELTA_BASE + 0 = 120
    { .name = "Delta2", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },            // IDX_DELTA_BASE + 1 = 121
    { .name = "Delta3", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },            // IDX_DELTA_BASE + 2 = 122
    { .name = "Delta4", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },            // IDX_DELTA_BASE + 3 = 123
    { .name = "Delta5", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },            // IDX_DELTA_BASE + 4 = 124
    { .name = "Delta6", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },            // IDX_DELTA_BASE + 5 = 125
    { .name = "Delta7", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },            // IDX_DELTA_BASE + 6 = 126
    { .name = "Delta8", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },            // IDX_DELTA_BASE + 7 = 127
    { .name = "Delta9", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },            // IDX_DELTA_BASE + 8 = 128
    { .name = "Delta10", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },           // IDX_DELTA_BASE + 9 = 129
    { .name = "Delta11", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },           // IDX_DELTA_BASE + 10 = 130
    { .name = "Delta12", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },           // IDX_DELTA_BASE + 11 = 131
    { .name = "Delta13", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },           // IDX_DELTA_BASE + 12 = 132
    { .name = "Delta14", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },           // IDX_DELTA_BASE + 13 = 133
    { .name = "Delta15", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },           // IDX_DELTA_BASE + 14 = 134
    { .name = "Delta16", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },           // IDX_DELTA_BASE + 15 = 135
    { .name = "DeltaAMT", .min = -127, .max = 127, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },       // IDX_DELTA_AMT = 136
    { .name = "DeltaScope", .min = 2, .max = 64, .def = 4, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },         // IDX_DELTA_SCOPE = 137
    { .name = "DeltaShape", .min = 0, .max = 3, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = deltaShapeEnums },  // IDX_DELTA_SHAPE = 138
    { .name = "DRMlengths", .min = 1, .max = 8, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_DRM_LENGTHS = 139 - Drum length preset
    { .name = "MorphX", .min = 0, .max = 127, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_MORPH_X = 140 - Morph X coordinate
    { .name = "MorphY", .min = 0, .max = 127, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_MORPH_Y = 141 - Morph Y coordinate
    { .name = "ZERO", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },              // IDX_ZERO = 142 - Zero trigger holes

};

// Page indices arrays (Expert Sleepers pattern: static const)
static const uint8_t pageRandIdx[] = {
    IDX_RANDOMIZE,
    IDX_RANDOMNESS,
    IDX_FREEZ,
    IDX_INCLUDE_BASE+0,  IDX_INCLUDE_BASE+1,  IDX_INCLUDE_BASE+2,  IDX_INCLUDE_BASE+3,
    IDX_INCLUDE_BASE+4,  IDX_INCLUDE_BASE+5,  IDX_INCLUDE_BASE+6,  IDX_INCLUDE_BASE+7,
    IDX_INCLUDE_BASE+8,  IDX_INCLUDE_BASE+9,  IDX_INCLUDE_BASE+10, IDX_INCLUDE_BASE+11,
    IDX_INCLUDE_BASE+12, IDX_INCLUDE_BASE+13, IDX_INCLUDE_BASE+14, IDX_INCLUDE_BASE+15
};
static const uint8_t pageMidiOutIdx[] = { IDX_MIDI_OUT };
static const uint8_t pageRouteIdx[] = { IDX_CLOCK_INPUT };
static const uint8_t pageParamIdx[] = {
    IDX_PARAM_BASE+0,  IDX_PARAM_BASE+1,  IDX_PARAM_BASE+2,  IDX_PARAM_BASE+3,
    IDX_PARAM_BASE+4,  IDX_PARAM_BASE+5,  IDX_PARAM_BASE+6,  IDX_PARAM_BASE+7,
    IDX_PARAM_BASE+8,  IDX_PARAM_BASE+9,  IDX_PARAM_BASE+10, IDX_PARAM_BASE+11,
    IDX_PARAM_BASE+12, IDX_PARAM_BASE+13, IDX_PARAM_BASE+14, IDX_PARAM_BASE+15,
    IDX_PARAM_BASE+16, IDX_PARAM_BASE+17, IDX_PARAM_BASE+18, IDX_PARAM_BASE+19,
    IDX_PARAM_BASE+20, IDX_PARAM_BASE+21, IDX_PARAM_BASE+22, IDX_PARAM_BASE+23,
    IDX_PARAM_BASE+24, IDX_PARAM_BASE+25, IDX_PARAM_BASE+26, IDX_PARAM_BASE+27,
    IDX_PARAM_BASE+28, IDX_PARAM_BASE+29, IDX_PARAM_BASE+30, IDX_PARAM_BASE+31,
    IDX_PARAM_BASE+32, IDX_PARAM_BASE+33, IDX_PARAM_BASE+34, IDX_PARAM_BASE+35,
    IDX_PARAM_BASE+36, IDX_PARAM_BASE+37, IDX_PARAM_BASE+38, IDX_PARAM_BASE+39,
    IDX_PARAM_BASE+40, IDX_PARAM_BASE+41, IDX_PARAM_BASE+42, IDX_PARAM_BASE+43,
    IDX_PARAM_BASE+44, IDX_PARAM_BASE+45, IDX_PARAM_BASE+46, IDX_PARAM_BASE+47,
    IDX_PARAM_BASE+48, IDX_PARAM_BASE+49, IDX_PARAM_BASE+50, IDX_PARAM_BASE+51,
    IDX_PARAM_BASE+52, IDX_PARAM_BASE+53, IDX_PARAM_BASE+54, IDX_PARAM_BASE+55,
    IDX_PARAM_BASE+56, IDX_PARAM_BASE+57, IDX_PARAM_BASE+58, IDX_PARAM_BASE+59,
    IDX_PARAM_BASE+60, IDX_PARAM_BASE+61, IDX_PARAM_BASE+62, IDX_PARAM_BASE+63
};
static const uint8_t pageTrigIdx[] = {
    IDX_TRIG_RND, IDX_TRIG_THRESH,
    IDX_TRIG_LEN_BASE+0, IDX_TRIG_LEN_BASE+1, IDX_TRIG_LEN_BASE+2, IDX_TRIG_LEN_BASE+3,
    IDX_TRIG_LEN_BASE+4, IDX_TRIG_LEN_BASE+5, IDX_TRIG_LEN_BASE+6, IDX_TRIG_LEN_BASE+7
};
static const uint8_t pageSeqIdx[] = {
    IDX_SEQ_PRESET, IDX_SEQ_TARGET, IDX_SEQ_CROSSFADE, IDX_SEQ_START_STOP,
    IDX_SEQ_TRACK_LENS+0, IDX_SEQ_TRACK_LENS+1, IDX_SEQ_TRACK_LENS+2,
    IDX_SEQ_TRACK_LENS+3, IDX_SEQ_TRACK_LENS+4, IDX_SEQ_TRACK_LENS+5
};
static const uint8_t pageOffsetIdx[] = {
    IDX_OFFSET_BASE+0, IDX_OFFSET_BASE+1, IDX_OFFSET_BASE+2,
    IDX_OFFSET_BASE+3, IDX_OFFSET_BASE+4, IDX_OFFSET_BASE+5,
    IDX_OFFSETS_ON, IDX_RND_OFFSETS, IDX_RND_OFF_AMT,
    IDX_BURST_RATE, IDX_BURST_COUNT, IDX_BURST_RND, IDX_BURST_RND_AMT
};
static const uint8_t pageDeltaIdx[] = {
    IDX_DELTA_BASE+0, IDX_DELTA_BASE+1, IDX_DELTA_BASE+2, IDX_DELTA_BASE+3,
    IDX_DELTA_BASE+4, IDX_DELTA_BASE+5, IDX_DELTA_BASE+6, IDX_DELTA_BASE+7,
    IDX_DELTA_BASE+8, IDX_DELTA_BASE+9, IDX_DELTA_BASE+10, IDX_DELTA_BASE+11,
    IDX_DELTA_BASE+12, IDX_DELTA_BASE+13, IDX_DELTA_BASE+14, IDX_DELTA_BASE+15,
    IDX_DELTA_AMT, IDX_DELTA_SCOPE, IDX_DELTA_SHAPE, IDX_DRM_LENGTHS,
    IDX_MORPH_X, IDX_MORPH_Y, IDX_ZERO
};

// Parameter pages (Expert Sleepers pattern: static const)
static const _NT_parameterPage pages[] = {
    { .name = "RAND",      .numParams = ARRAY_SIZE(pageRandIdx),    .params = pageRandIdx },
    { .name = "MIDI out",  .numParams = ARRAY_SIZE(pageMidiOutIdx), .params = pageMidiOutIdx },
    { .name = "ROUTING",   .numParams = ARRAY_SIZE(pageRouteIdx),   .params = pageRouteIdx },
    { .name = "PARAM",     .numParams = ARRAY_SIZE(pageParamIdx),   .params = pageParamIdx },
    { .name = "TRIGGERS",  .numParams = ARRAY_SIZE(pageTrigIdx),    .params = pageTrigIdx },
    { .name = "SEQUENCER", .numParams = ARRAY_SIZE(pageSeqIdx),     .params = pageSeqIdx },
    { .name = "OFFSETS",   .numParams = ARRAY_SIZE(pageOffsetIdx),  .params = pageOffsetIdx },
    { .name = "DELTA",     .numParams = ARRAY_SIZE(pageDeltaIdx),   .params = pageDeltaIdx }
};

static const _NT_parameterPages parameterPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages = pages
};

// ----- Plugin state -----
struct Plugin : _NT_algorithm {
    // CC sequences - 4 sequences per track for 2D morphing
    // [track][step][sequence] where sequence 0=LL, 1=LR, 2=UL, 3=UR
    uint8_t ccVals[MAX_SEQS][MAX_STEPS][4];
    uint8_t pos[MAX_SEQS], divCount[MAX_SEQS];
    uint8_t steps[MAX_SEQS], divs[MAX_SEQS], ranges[MAX_SEQS], dirs[MAX_SEQS];
    bool    randomize;
    uint8_t randomness;
    bool    freez;                          // NEW: Global CC freeze state
    uint8_t frozenCcVals[MAX_SEQS];        // NEW: Frozen CC values for sample-hold
    bool    includes[MAX_SEQS];
    float   lastClock;
    uint32_t globalClockCount;             // NEW: Absolute clock counter (never resets)

    // 2D Morphing parameters
    uint8_t morphX;                        // Morph X coordinate (0-127)
    uint8_t morphY;                        // Morph Y coordinate (0-127)

    // Original Triggers
    uint8_t trigWeights[TRIG_SEQS][TRIG_STEPS];
    uint8_t trigPos[TRIG_SEQS];
    uint8_t trigLen[TRIG_SEQS];
    uint8_t trigThresh;
    bool    trigRnd;
    float   lastClockTrig;

    // ZERO functionality - mask for silencing trigger steps
    uint8_t trigZeroMask[TRIG_SEQS][TRIG_STEPS];  // 1 = allow trigger, 0 = zeroed/silenced

    // Original MIDI trigger state
    uint32_t pendingMidi;

    // NEW: Step Sequencer state
    uint8_t seqSteps[SEQ_TRACKS][TRIG_STEPS];  // Step on/off states (loaded from presets)
    uint8_t seqPos[SEQ_TRACKS];                // Current position per track
    uint8_t seqLen[SEQ_TRACKS];                // Length per track (1-16)
    uint8_t seqCurrentPreset;                  // Current preset index
    uint8_t seqTargetPreset;                   // Target preset for crossfade
    uint8_t seqCrossfade;                      // Crossfade amount (0-127)
    bool    seqStartStop;                      // Start/stop control

    // Tracks 7-8 have fixed patterns (not part of presets)
    uint8_t seqTrack7Pos;                      // Track 7: 32-step counter
    uint8_t seqTrack8Pos;                      // Track 8: 128-step counter
    bool    seqTrack8Playing;                  // Track 8: gate state (true when held high)

    // NEW: Sub-step offsets for tracks 1-6 (-8 to +8)
    int8_t  seqOffsets[6];                     // Sub-step offsets for each track
    bool    offsetsOn;                         // Enable/disable offsets
    uint8_t lastRndOffsets;                    // Previous RNDOffsets value for edge detection
    uint8_t rndOffAmt;                         // Randomization amount (0-8)

    // Delta LFO for CC modulation
    bool     deltaEnabled[MAX_SEQS];           // Enable/disable Delta per track
    int8_t   deltaAmt;                         // Delta amount (-127 to +127)
    uint8_t  deltaScope;                       // Delta scope (2-64 repetitions)
    uint8_t  deltaShape;                       // Delta shape (0=Ramp, 1=Saw, 2=Triangle, 3=Square)
    uint32_t deltaRepetitionCounter[MAX_SEQS]; // Repetition counter per track

    // Burst generator on Track 2 (Sub) - MIDI Note 9, Channel 4
    // Triggered by snare (Track 5), clocked by phase accumulator
    uint8_t  burstRate;                        // Burst rate in 0.1 Hz (10-200 = 1.0-20.0 Hz)
    uint8_t  burstCount;                       // Number of bursts (0-8)
    bool     burstRnd;                         // Randomization enabled
    uint8_t  burstRndAmt;                      // Randomization amount (0-100%)
    uint32_t burstPhaseAccumulator;            // 32-bit phase accumulator (0 to 2^32-1)
    uint32_t burstPhaseIncrement;              // Phase increment per sample
    uint32_t burstRandomOffsets[8];            // 8-pattern random timing offsets
    uint8_t  burstCurrentCount;                // Current burst number (0-7)
    bool     burstActive;                      // Burst sequence active
    uint16_t burstPulseCounter;                // Sample counter for 4ms pulse duration
    bool     burstPulseState;                  // Current pulse state (high/low)

    // 1/16 clock output (CC#1, Channel 7)
    uint16_t clockPulseCounter;                // Sample counter for 15ms clock pulse
};

// ----- Generate CC sequences -----
// Generates 4 random sequences per track for 2D morphing
static void genSequences(Plugin* p) {
    for(int i=0; i<MAX_SEQS; ++i) {
        if(p->includes[i]) {
            for(int s=0; s<p->steps[i]; ++s) {
                // Generate 4 sequences: LL (0), LR (1), UL (2), UR (3)
                for(int seq=0; seq<4; ++seq) {
                    p->ccVals[i][s][seq] = rand() % (p->ranges[i] + 1);
                }
            }
        }
    }
}

// ----- Bilinear interpolation for 2D morphing -----
// Interpolates between 4 sequences based on morph X/Y coordinates
static uint8_t getMorphedCCValue(Plugin* p, int track, int step) {
    // Get the 4 corner values
    uint8_t A = p->ccVals[track][step][0];  // Lower-left  (x=0, y=0)
    uint8_t B = p->ccVals[track][step][1];  // Lower-right (x=1, y=0)
    uint8_t C = p->ccVals[track][step][2];  // Upper-left  (x=0, y=1)
    uint8_t D = p->ccVals[track][step][3];  // Upper-right (x=1, y=1)

    // Normalize morph coordinates to 0.0-1.0
    float x = (float)p->morphX / 127.0f;
    float y = (float)p->morphY / 127.0f;

    // Bilinear interpolation
    float bottom = (float)A * (1.0f - x) + (float)B * x;
    float top    = (float)C * (1.0f - x) + (float)D * x;
    float result = bottom * (1.0f - y) + top * y;

    // Clamp to 0-127 range and return
    if (result < 0.0f) result = 0.0f;
    if (result > 127.0f) result = 127.0f;

    return (uint8_t)result;
}

// ----- Generate burst random offsets -----
static void generateBurstRandomOffsets(Plugin* p) {
    for (int i = 0; i < 8; ++i) {
        if (p->burstRnd && p->burstRndAmt > 0) {
            // Generate random offset as percentage of phase increment
            // Range: ±burstRndAmt% of the base phase increment
            int32_t maxOffset = (int32_t)((p->burstPhaseIncrement * (uint32_t)p->burstRndAmt) / 100);
            int32_t randomOffset = (rand() % (maxOffset * 2 + 1)) - maxOffset;
            p->burstRandomOffsets[i] = (uint32_t)randomOffset;
        } else {
            p->burstRandomOffsets[i] = 0;
        }
    }
}

// ----- Update burst phase parameters -----
static void updateBurstPhase(Plugin* p) {
    // Use floating-point to calculate phase increment precisely
    // Phase increment = (2^32 / sampleRate) * frequency
    // burstRate is stored as 10-200 representing 1.0-20.0 Hz, so divide by 10
    float frequencyHz = (float)p->burstRate / 10.0f;
    float phaseIncPerSample = (4294967296.0f / NT_globals.sampleRate) * frequencyHz;
    p->burstPhaseIncrement = (uint32_t)phaseIncPerSample;

    // Regenerate random offsets whenever phase changes
    generateBurstRandomOffsets(p);
}

// ----- Calculate Delta LFO value for a given track -----
static int8_t calculateDeltaValue(Plugin* p, int trackIndex) {
    if (!p->deltaEnabled[trackIndex] || p->deltaAmt == 0 || p->deltaScope == 0) {
        return 0;
    }

    // Calculate position within the Delta cycle (0 to deltaScope-1)
    uint32_t cyclePosition = p->deltaRepetitionCounter[trackIndex] % p->deltaScope;

    // Calculate normalized position (0.0 to 1.0)
    float normalizedPos = (float)cyclePosition / (float)p->deltaScope;

    // Calculate waveform value based on shape
    float waveformValue = 0.0f;

    switch (p->deltaShape) {
        case 0: // Ramp: 0 → DeltaAMT
            waveformValue = normalizedPos;
            break;

        case 1: // Saw: DeltaAMT → 0
            waveformValue = 1.0f - normalizedPos;
            break;

        case 2: // Triangle: 0 → DeltaAMT → 0
            if (normalizedPos < 0.5f) {
                waveformValue = normalizedPos * 2.0f;  // Rising edge
            } else {
                waveformValue = (1.0f - normalizedPos) * 2.0f;  // Falling edge
            }
            break;

        case 3: // Square: 0 for first half, DeltaAMT for second half
            waveformValue = (normalizedPos < 0.5f) ? 0.0f : 1.0f;
            break;
    }

    // Scale by DeltaAMT and return
    return (int8_t)(waveformValue * (float)p->deltaAmt);
}

// ----- Generate Trigger weights -----
static void genTrig(Plugin* p) {
    for(int i=0; i<TRIG_SEQS; ++i) {
        if(p->trigRnd) {
            for(int s=0; s<TRIG_STEPS; ++s) {
                p->trigWeights[i][s] = (rand() % 128) + 1;
                p->trigZeroMask[i][s] = 1;  // Clear zeros when randomizing
            }
        }
        p->trigPos[i] = 0;
    }
}

// ----- Load Preset -----
static void loadPreset(Plugin* p, uint8_t presetIdx) {
    if (presetIdx >= NUM_PRESETS) return;

    // Only load tracks 1-6 (indices 0-5) - tracks 7-8 have fixed patterns
    for(int t=0; t<6; ++t) {
        for(int s=0; s<16; ++s) {
            p->seqSteps[t][s] = SEQUENCE_PRESETS[presetIdx][t][s];
        }
    }
}

// ----- Initialize Step Sequencer -----
static void initStepSeq(Plugin* p) {
    for(int t=0; t<SEQ_TRACKS; ++t) {
        p->seqLen[t] = 16;
        p->seqPos[t] = 0;
    }
    p->seqCurrentPreset = 0;
    p->seqTargetPreset = 0;
    p->seqCrossfade = 0;
    p->seqStartStop = true;

    // Initialize tracks 7-8 counters
    p->seqTrack7Pos = 0;      // 32-step counter for track 7
    p->seqTrack8Pos = 0;      // 128-step counter for track 8
    p->seqTrack8Playing = false;  // Track 8 gate state

    // Load initial preset
    loadPreset(p, 0);
}


// ----- Audio step: 1 pattern step per rising clock edge -----
extern "C" void pluginStep(_NT_algorithm* alg, float* busFrames, int numFramesBy4) {
    Plugin* p = (Plugin*)alg;

    const int numFrames = numFramesBy4 * 4;
    const int busParam  = alg->v[IDX_CLOCK_INPUT];

    // Pre-compute clock bus pointer once before loop (Chords pattern)
    const float* clockIn = (busParam >= 1 && busParam <= 28)
                           ? busFrames + (busParam - 1) * numFrames : nullptr;

    const uint32_t dest = alg->v[IDX_MIDI_OUT]
                        ? (uint32_t)kNT_destinationBreakout
                        : (uint32_t)kNT_destinationUSB;

    for (int frame = 0; frame < numFrames; ++frame) {

        if (clockIn) {
            const float clk = clockIn[frame];

            // Rising edge: was below 1V, now at or above 1V — no hysteresis dead zone
            if (p->lastClock < 1.0f && clk >= 1.0f) {

                // --- Drum tracks 1-6: direct position indexing ---
                if (p->seqStartStop) {
                    for (int t = 0; t < 6; ++t) {
                        uint8_t pos     = p->seqPos[t];
                        uint8_t curStep = SEQUENCE_PRESETS[p->seqCurrentPreset][t][pos];
                        uint8_t tgtStep = SEQUENCE_PRESETS[p->seqTargetPreset][t][pos];

                        uint8_t stepToPlay;
                        if (p->seqCrossfade == 0) {
                            stepToPlay = curStep;
                        } else if (p->seqCrossfade >= 127) {
                            stepToPlay = tgtStep;
                        } else {
                            uint8_t randomValue = rand() % 128;
                            stepToPlay = (randomValue < p->seqCrossfade) ? tgtStep : curStep;
                        }

                        // Skip Track 2 (t=1) — controlled by burst generator
                        if (stepToPlay && t != 1) {
                            NT_sendMidi3ByteMessage(dest, 0x90 | 3, (uint8_t)(t + 8), 127);

                            // Track 5 (snare, t=4) triggers burst
                            if (t == 4 && p->burstCount > 0) {
                                p->burstActive = true;
                                p->burstCurrentCount = 0;
                                p->burstPhaseAccumulator = 0;
                            }
                        }

                        p->seqPos[t] = (p->seqPos[t] + 1) % p->seqLen[t];
                    }

                    // Tracks 7 & 8
                    if (p->seqTrack7Pos == 0)
                        NT_sendMidi3ByteMessage(dest, 0x90 | 3, 14, 127);
                    p->seqTrack7Pos = (p->seqTrack7Pos + 1) % 32;

                    if (p->seqTrack8Pos == 120 && !p->seqTrack8Playing) {
                        NT_sendMidi3ByteMessage(dest, 0x90 | 3, 15, 127);
                        p->seqTrack8Playing = true;
                    } else if (p->seqTrack8Pos == 127 && p->seqTrack8Playing) {
                        NT_sendMidi3ByteMessage(dest, 0x80 | 3, 15, 0);
                        p->seqTrack8Playing = false;
                    }
                    p->seqTrack8Pos = (p->seqTrack8Pos + 1) % 128;
                }

                // --- Trigger sequences ---
                for (int i = 0; i < TRIG_SEQS; ++i) {
                    if (p->trigPos[i] < p->trigLen[i]) {
                        if (alg->v[IDX_ZERO] == 1)
                            p->trigZeroMask[i][p->trigPos[i]] = 0;
                        uint8_t w = p->trigWeights[i][p->trigPos[i]];
                        if (w >= p->trigThresh && p->trigZeroMask[i][p->trigPos[i]] == 1)
                            NT_sendMidi3ByteMessage(dest, 0x90 | 1, (uint8_t)i, 127);
                        p->trigPos[i] = (p->trigPos[i] + 1) % p->trigLen[i];
                    }
                }

                // --- CC sequences ---
                for (int i = 0; i < MAX_SEQS; ++i) {
                    if (++p->divCount[i] >= p->divs[i]) {
                        p->divCount[i] = 0;

                        uint8_t val;
                        if (p->freez) {
                            val = p->frozenCcVals[i];
                        } else if (p->randomize) {
                            val = getMorphedCCValue(p, i, p->pos[i]);
                        } else {
                            if (p->randomness > 0) {
                                uint8_t random_chance = rand() % 128;
                                if (random_chance < p->randomness) {
                                    for (int seq = 0; seq < 4; ++seq)
                                        p->ccVals[i][p->pos[i]][seq] = rand() % (p->ranges[i] + 1);
                                }
                            }
                            for (int seq = 0; seq < 4; ++seq) {
                                if (p->ccVals[i][p->pos[i]][seq] > p->ranges[i])
                                    p->ccVals[i][p->pos[i]][seq] = p->ranges[i];
                            }
                            val = getMorphedCCValue(p, i, p->pos[i]);

                            uint8_t oldPos = p->pos[i];
                            switch (p->dirs[i]) {
                                case 0: p->pos[i] = (p->pos[i] + 1) % p->steps[i]; break;
                                case 1: p->pos[i] = p->pos[i] ? p->pos[i] - 1 : p->steps[i] - 1; break;
                                case 2: p->pos[i] = rand() % p->steps[i]; break;
                            }
                            bool seqDone = (p->dirs[i] == 0 && oldPos == p->steps[i] - 1)
                                        || (p->dirs[i] == 1 && oldPos == 0);
                            if (seqDone) p->deltaRepetitionCounter[i]++;
                        }

                        int8_t deltaValue = p->freez ? (int8_t)0 : calculateDeltaValue(p, i);
                        int16_t finalValue = (int16_t)val + (int16_t)deltaValue;
                        if (finalValue < 0) finalValue = 0;
                        if (finalValue > 127) finalValue = 127;

                        NT_sendMidi3ByteMessage(dest, 0xB0 | 2, 17 + i, (uint8_t)finalValue);
                    }
                }

                // 1/16 clock output pulse
                NT_sendMidi3ByteMessage(dest, 0xB0 | 6, 1, 127);
                p->clockPulseCounter = (uint16_t)(NT_globals.sampleRate * 0.015f);

                p->globalClockCount++;
            }

            // Always store raw clock value — no dead zone (Chords pattern)
            p->lastClock = clk;
        }

        // --- Burst generator ---
        if (p->burstActive) {
            uint32_t oldPhase = p->burstPhaseAccumulator;
            p->burstPhaseAccumulator += p->burstPhaseIncrement
                                      + p->burstRandomOffsets[p->burstCurrentCount % 8];
            if (oldPhase > p->burstPhaseAccumulator) {
                if (p->burstCurrentCount < p->burstCount) {
                    p->burstPulseState   = true;
                    p->burstPulseCounter = (uint16_t)(NT_globals.sampleRate * 0.004f);
                    NT_sendMidi3ByteMessage(dest, 0x90 | 3, 9, 127);
                    if (++p->burstCurrentCount >= p->burstCount)
                        p->burstActive = false;
                }
            }
        }

        if (p->burstPulseState && p->burstPulseCounter > 0) {
            if (--p->burstPulseCounter == 0) {
                p->burstPulseState = false;
                NT_sendMidi3ByteMessage(dest, 0x80 | 3, 9, 0);
            }
        }

        if (p->clockPulseCounter > 0) {
            if (--p->clockPulseCounter == 0)
                NT_sendMidi3ByteMessage(dest, 0xB0 | 6, 1, 0);
        }

    }  // end frame loop
}

// ----- Parameter changed -----
extern "C" void parameterChanged(_NT_algorithm* alg, int idx) {
    Plugin* p = (Plugin*)alg;
    
    // Original parameter handling
    if (idx == IDX_RANDOMIZE) {
        bool newRandomize = alg->v[idx];
        // INVERTED LOGIC: Randomize on 1→0, hold on 0→1
        if (!newRandomize && p->randomize) {
            // Going from 1 to 0: Generate new random sequences
            genSequences(p);
        }
        // When going from 0 to 1: Do nothing (hold current values)
        p->randomize = newRandomize;
    }
    else if (idx == IDX_RANDOMNESS) {
        p->randomness = alg->v[idx];
    }
    else if (idx == IDX_FREEZ) {
        bool newFreezState = alg->v[idx];
        if (newFreezState && !p->freez) {
            // Going from 0 to 1: Capture current morphed CC values
            for (int i = 0; i < MAX_SEQS; ++i) {
                p->frozenCcVals[i] = getMorphedCCValue(p, i, p->pos[i]);
            }
        }
        p->freez = newFreezState;
    }
    else if (idx >= IDX_INCLUDE_BASE && idx < IDX_INCLUDE_BASE + MAX_SEQS) {
        p->includes[idx - IDX_INCLUDE_BASE] = alg->v[idx];
    }
    else if (idx == IDX_CLOCK_INPUT) {
        p->lastClock = p->lastClockTrig = 0.0f;
    }
    else if (idx >= IDX_PARAM_BASE && idx < CC_NUM_PARAMS) {
        int seq = (idx - IDX_PARAM_BASE) / 4, f = (idx - IDX_PARAM_BASE) % 4;
        int16_t v = alg->v[idx];
        if (f == 0) p->steps[seq]  = v;
        if (f == 1) p->divs[seq]   = v;
        if (f == 2) p->ranges[seq] = v;
        if (f == 3) p->dirs[seq]   = v;
        if (p->randomize && p->includes[seq]) {
            for (int s = 0; s < p->steps[seq]; ++s) {
                for (int seqIdx = 0; seqIdx < 4; ++seqIdx) {
                    p->ccVals[seq][s][seqIdx] = rand() % (p->ranges[seq] + 1);
                }
            }
        }
    }
    else if (idx == IDX_TRIG_RND) {
        p->trigRnd = alg->v[idx];
        if (p->trigRnd) genTrig(p);
    }
    else if (idx == IDX_TRIG_THRESH) {
        p->trigThresh = alg->v[idx];
    }
    else if (idx >= IDX_TRIG_LEN_BASE && idx < IDX_TRIG_LEN_BASE + TRIG_SEQS) {
        p->trigLen[idx - IDX_TRIG_LEN_BASE] = alg->v[idx];
    }
    
    // NEW: Step Sequencer parameter handling
    else if (idx == IDX_SEQ_PRESET) {
        p->seqCurrentPreset = alg->v[idx] - 1;  // Convert GUI value (1-16) to array index (0-15)
        loadPreset(p, p->seqCurrentPreset);
    }
    else if (idx == IDX_SEQ_TARGET) {
        p->seqTargetPreset = alg->v[idx] - 1;  // Convert GUI value (1-16) to array index (0-15)
    }
    else if (idx == IDX_SEQ_CROSSFADE) {
        p->seqCrossfade = alg->v[idx];
    }
    else if (idx == IDX_SEQ_START_STOP) {
        bool newState = alg->v[idx];
        if (newState && !p->seqStartStop) {
            // Starting: reset all sequencer positions and global clock
            for (int t = 0; t < SEQ_TRACKS; ++t) {
                p->seqPos[t] = 0;
            }
            // Reset tracks 7 & 8 to their first steps
            p->seqTrack7Pos = 0;  // Track 7 starts at step 1 of 32
            p->seqTrack8Pos = 0;  // Track 8 starts at step 1 of 128
            p->seqTrack8Playing = false;  // Reset track 8 gate state
            p->globalClockCount = 0;  // Reset global clock for fresh start
        }
        p->seqStartStop = newState;
    }
    else if (idx >= IDX_SEQ_TRACK_LENS && idx < IDX_SEQ_TRACK_LENS + 6) {
        p->seqLen[idx - IDX_SEQ_TRACK_LENS] = alg->v[idx];
    }

    // NEW: Sub-step offset parameter handling
    else if (idx >= IDX_OFFSET_BASE && idx < IDX_OFFSET_BASE + 6) {
        p->seqOffsets[idx - IDX_OFFSET_BASE] = alg->v[idx];
    }
    else if (idx == IDX_OFFSETS_ON) {
        p->offsetsOn = (alg->v[idx] > 0);
    }
    else if (idx == IDX_RND_OFFSETS) {
        // Edge detection: trigger randomization on 0->1 transition
        uint8_t newValue = alg->v[idx];
        if (newValue > 0 && p->lastRndOffsets == 0) {
            // Randomize all 6 offsets within the specified range
            // Note: The randomized values are stored in p->seqOffsets[] and will be used for triggering.
            // The parameter display will reflect these when the user navigates to those parameters.
            for (int i = 0; i < 6; ++i) {
                int range = p->rndOffAmt;
                // Generate random value from -range to +range
                int randomOffset = (rand() % (2 * range + 1)) - range;
                p->seqOffsets[i] = randomOffset;
            }
        }
        p->lastRndOffsets = newValue;
    }
    else if (idx == IDX_RND_OFF_AMT) {
        p->rndOffAmt = alg->v[idx];
    }
    else if (idx == IDX_BURST_RATE) {
        p->burstRate = alg->v[idx];
        updateBurstPhase(p);  // Recalculate phase increment and random offsets
    }
    else if (idx == IDX_BURST_COUNT) {
        uint8_t newCount = alg->v[idx];
        // BCount going from 0→1 triggers burst randomization
        if (newCount == 1 && p->burstCount == 0) {
            generateBurstRandomOffsets(p);
        }
        p->burstCount = newCount;
        // If set to 0, stop any active burst
        if (p->burstCount == 0) {
            p->burstActive = false;
        }
    }
    else if (idx == IDX_BURST_RND) {
        p->burstRnd = (alg->v[idx] > 0);
        generateBurstRandomOffsets(p);  // Regenerate pattern when toggled
    }
    else if (idx == IDX_BURST_RND_AMT) {
        p->burstRndAmt = alg->v[idx];
        generateBurstRandomOffsets(p);  // Regenerate pattern when amount changes
    }
    else if (idx >= IDX_DELTA_BASE && idx < IDX_DELTA_BASE + 16) {
        int trackIdx = idx - IDX_DELTA_BASE;
        p->deltaEnabled[trackIdx] = (alg->v[idx] > 0);
    }
    else if (idx == IDX_DELTA_AMT) {
        p->deltaAmt = (int8_t)alg->v[idx];
    }
    else if (idx == IDX_DELTA_SCOPE) {
        p->deltaScope = alg->v[idx];
    }
    else if (idx == IDX_DELTA_SHAPE) {
        p->deltaShape = alg->v[idx];
    }
    else if (idx == IDX_DRM_LENGTHS) {
        // Apply drum length preset to tracks 1-6
        uint8_t preset = alg->v[idx] - 1;  // Convert 1-8 to 0-7 array index
        if (preset < 8) {
            for (int i = 0; i < 6; ++i) {
                p->seqLen[i] = DRUM_LENGTH_PRESETS[preset][i];
            }
        }
    }
    else if (idx == IDX_MORPH_X) {
        p->morphX = alg->v[idx];
    }
    else if (idx == IDX_MORPH_Y) {
        p->morphY = alg->v[idx];
    }
    else if (idx == IDX_ZERO) {
        // ZERO masking now happens continuously in playback loop
        // No action needed on parameter change
    }
}

// ----- Requirements -----
extern "C" void calculateRequirements(_NT_algorithmRequirements& r, const int32_t*) {
    r.numParameters = ARRAY_SIZE(parameters);  // Expert Sleepers pattern
    r.sram          = sizeof(Plugin);
    r.dram = r.dtc = r.itc = 0;
}

// ----- Construct -----
extern "C" _NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs,
                                      const _NT_algorithmRequirements&,
                                      const int32_t*) {

    // Allocate plugin in SRAM
    Plugin* p = new(ptrs.sram) Plugin();

    // EXPERT SLEEPERS APPROACH: Minimal initialization - let disting NT set parameter defaults
    // Only initialize non-parameter state
    p->lastClock     = 0.0f;
    p->lastClockTrig = 0.0f;
    p->globalClockCount = 0;  // NEW: Initialize global clock counter

    // Initialize positions and counters (not parameters)
    for (int i = 0; i < MAX_SEQS; ++i) {
        p->pos[i] = 0;
        p->divCount[i] = 0;
        p->frozenCcVals[i] = 0;  // Initialize frozen CC values
    }
    for (int i = 0; i < TRIG_SEQS; ++i) {
        p->trigPos[i] = 0;
    }

    // NEW: Initialize sub-step offsets (parameters will be loaded by disting NT)
    for (int i = 0; i < 6; ++i) {
        p->seqOffsets[i] = 0;
    }
    p->offsetsOn = true;           // Offsets enabled by default
    p->lastRndOffsets = 0;         // Initialize edge detection
    p->rndOffAmt = 8;              // Full range by default

    // Initialize burst generator on Track 2 (MIDI)
    p->burstRate = 100;              // Default 10.0 Hz (stored as 100 = 10.0 * 10)
    p->burstCount = 4;              // Default 4 bursts
    p->burstRnd = false;            // Randomization off by default
    p->burstRndAmt = 50;            // Default 50% randomization
    p->burstPhaseAccumulator = 0;   // Start at phase 0
    p->burstCurrentCount = 0;       // No bursts fired yet
    p->burstActive = false;         // Not active
    p->burstPulseCounter = 0;       // No active pulse
    p->burstPulseState = false;     // Start low (Note OFF)
    for (int i = 0; i < 8; ++i) {
        p->burstRandomOffsets[i] = 0;  // Initialize random offsets
    }
    updateBurstPhase(p);            // Calculate initial phase increment and offsets

    // Initialize 1/16 clock output
    p->clockPulseCounter = 0;       // No active clock pulse

    // Initialize Delta LFO
    for (int i = 0; i < MAX_SEQS; ++i) {
        p->deltaEnabled[i] = false;
        p->deltaRepetitionCounter[i] = 0;
    }
    p->deltaAmt = 0;
    p->deltaScope = 4;
    p->deltaShape = 0;  // Ramp

    // Initialize 2D Morphing
    p->morphX = 0;  // Lower-left corner by default
    p->morphY = 0;

    // Initialize ZERO functionality
    for (int i = 0; i < TRIG_SEQS; ++i) {
        for (int s = 0; s < TRIG_STEPS; ++s) {
            p->trigZeroMask[i][s] = 1;  // All steps enabled by default
        }
    }

    // Initialize new systems (non-parameter state)
    initStepSeq(p);

    _NT_algorithm* alg = (_NT_algorithm*)p;
    alg->parameters     = parameters;        // Expert Sleepers: direct assignment of static const array
    alg->parameterPages = &parameterPages;

    // parameterChanged() is not called at construction by the disting NT firmware.
    // Call it explicitly for every parameter so all cached fields are initialised
    // from the saved/default values before audio starts.
    for (int i = 0; i < NUM_PARAMS; ++i)
        parameterChanged(alg, i);

    return alg;
}

// ----- Factory -----
extern "C" uintptr_t pluginEntry(_NT_selector s, uint32_t) {
    switch (s) {
        case kNT_selector_version:      return kNT_apiVersionCurrent;
        case kNT_selector_numFactories: return 1;
        case kNT_selector_factoryInfo: {
            static _NT_factory f = {
                NT_MULTICHAR('B','Z','M','S'),
                "BizmuthSequencer",
                "8x clock sequencer with sub-step offsets",
                0, nullptr,
                nullptr, nullptr,
                calculateRequirements,
                construct,
                parameterChanged,  // Keep parameterChanged - essential for plugin functionality
                pluginStep,
                nullptr, nullptr, nullptr,
                kNT_tagUtility,
                nullptr, nullptr, nullptr
            };
            return (uintptr_t)&f;
        }
    default: return 0 ;
    }
}