// BizmuthSeq v5
// Drum+CC sequencer with shuffle clock and recorder.
// Rewrites v4 following v3 code style (extern "C" linkage, ARRAY_SIZE pattern).
//
// Changes from v3 (8x clock, 143 params, GUID BZMS):
//   - New GUID BZM5 — avoids preset conflicts with v3's 143-parameter saves
//   - 67 parameters, renumbered from scratch
//   - 1 step per rising edge (8x sub-step system, offsets, burst, delta all removed)
//   - Drums: 5ms bus pulses on buses 37-44 (Kick/Sub/Sub2/OH/Snare/CH/Ride/Clap)
//   - MIDI: DIN + Internal simultaneously
//   - Removed: Include 1-16, Div 1-16, Dir 1-16, MIDI Out selection
//   - Removed: Offset page, Burst generator, Delta LFO system
//   - Removed: Seq Len 1-6 individual params
//   - DRMlengths preset now covers all 8 tracks (was 6)
//   - Added: ShuffleClockIn, RecClock to ROUTING page
//   - Added: SHUFFLE page — per-drum ON/OFF clock source (OFF=main, ON=shuffle)
//   - Added: RECORDER page — Record ON/OFF, RecLength 1-8 bars, Live! trigger
//   - Pattern 17 = recorded MIDI drum loop; crossfade uses probability gating
//   - CH10 notes 0-7 on incoming MIDI → 5ms pulse on drum buses 37-44
//   - Tracks 7-8 (Ride, Clap) have empty patterns — fill later

#include "distingnt/api.h"
#include <new>
#include <cstdlib>
#include <cstring>

#define MAX_SEQS        16
#define MAX_STEPS       16
#define TRIG_SEQS        8
#define TRIG_STEPS      16
#define SEQ_TRACKS       8
#define NUM_PRESETS     16
#define MAX_REC_EVENTS  2048

// ----- Recorder modes -----
enum RecMode : uint8_t {
    REC_LIVE      = 0,
    REC_RECORDING = 1,
    REC_OVERDUB   = 2,
    REC_PLAYBACK  = 3
};

// ----- Parameter indices -----
enum {
    // RAND page (0-2)
    IDX_RANDOMIZE     = 0,   // Randomise! (ON/OFF)
    IDX_RANDOMNESS    = 1,   // RandomNESS (0-127)
    IDX_FREEZ         = 2,   // FREEZ! (ON/OFF)

    // ROUTING page (3-5)
    IDX_CLOCK_INPUT   = 3,   // Clock input (bus 1-64, def 21)
    IDX_SHUFFLE_CLK   = 4,   // ShuffleClockIn (bus 0-64, def 0; 0=not connected)
    IDX_REC_CLOCK     = 5,   // RecClock (bus 0-64, def 0; 0=recorder disabled)

    // PARAM page: Steps 1-16 (6-21)
    IDX_STEPS_BASE    = 6,   // Steps 1 = 6, Steps 2 = 7, ..., Steps 16 = 21

    // PARAM page: Range 1-16 (22-37)
    IDX_RANGE_BASE    = 22,  // Range 1 = 22, Range 2 = 23, ..., Range 16 = 37

    // TRIGGERS page (38-47)
    IDX_TRIG_RND      = 38,  // Triggers RND (ON/OFF)
    IDX_TRIG_THRESH   = 39,  // Threshold (1-128, def 90)
    IDX_TRIG_LEN_BASE = 40,  // Length 1 = 40, ..., Length 8 = 47

    // SEQUENCER page (48-55)
    IDX_SEQ_PRESET    = 48,  // Preset (1-17; 17 = recording)
    IDX_SEQ_TARGET    = 49,  // Target (1-17)
    IDX_SEQ_CROSSFADE = 50,  // Crossfade (0-127)
    IDX_SEQ_START_STOP = 51, // Start/Stop (ON/OFF, def ON)
    IDX_DRM_LENGTHS   = 52,  // DRMlengths (1-8 preset index)
    IDX_MORPH_X       = 53,  // MorphX (0-127)
    IDX_MORPH_Y       = 54,  // MorphY (0-127)
    IDX_ZERO          = 55,  // ZERO (ON/OFF — creates silent holes in trigger output)

    // SHUFFLE page (56-63)
    IDX_SHUFFLE_BASE  = 56,  // Skick=56, Ssub=57, Ssub2=58, Soh=59,
                             // Ssnare=60, Sch=61, Sride=62, Sclap=63

    // RECORDER page (64-66)
    IDX_RECORD        = 64,  // Record (ON/OFF)
    IDX_REC_LENGTH    = 65,  // RecLength (1-8 bars; 1 bar = 16 RecClock steps)
    IDX_LIVE          = 66,  // Live! (ON/OFF; rising edge = delete, return to live)

    NUM_PARAMS        = 67
};

// ----- UI enum strings -----
static const char* enumOnOff[] = { "OFF", "ON", nullptr };

// ----- Drum Length Presets (8 presets x 8 tracks) -----
// Track order: Kick, Sub, Sub2, OH, Snare, CH, Ride, Clap
static const uint8_t DRUM_LENGTH_PRESETS[8][8] = {
    {16,16,16,16,16,16,16,16},  // 1: All 16
    {12,12, 4, 6,10, 8,16,16},  // 2
    {10,10, 6, 4, 6, 8,16,16},  // 3
    {14,14,10, 8, 6, 4,16,16},  // 4
    {14,14,12, 8,10, 6,16,16},  // 5
    { 7, 7,14, 8, 6,12,16,16},  // 6
    { 7, 7, 9, 9, 6, 5,16,16},  // 7
    {14,14,12, 4, 7, 8,16,16}   // 8
};

// ----- Sequence Presets (16 presets, tracks 0-5 = Kick/Sub/Sub2/OH/Snare/CH) -----
// Tracks 6-7 (Ride, Clap) are always zero — to be filled later.
static const uint8_t SEQUENCE_PRESETS[NUM_PRESETS][6][16] = {
    // Preset 1
    {{1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},   // Kick
     {0,0,1,0,0,1,0,0,0,0,0,1,0,1,0,0},   // Sub
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},   // Sub2
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},   // OH
     {0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0},   // Snare
     {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}},  // CH
    // Preset 2
    {{1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},
     {0,1,0,1,0,0,1,0,0,1,0,1,0,0,1,0},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
     {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0},
     {0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1}},
    // Preset 3
    {{1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},
     {0,0,1,0,0,1,0,1,0,0,1,0,0,1,0,1},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
     {0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0},
     {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}},
    // Preset 4
    {{1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},
     {0,0,0,1,0,0,1,0,0,0,0,1,0,0,1,1},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
     {0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0},
     {0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1}},
    // Preset 5
    {{1,0,0,1,0,0,1,0,0,0,1,0,0,0,1,1},
     {0,1,0,0,0,0,0,0,0,0,0,1,0,0,1,0},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
     {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0},
     {1,1,1,1,1,1,1,1,1,1,0,1,1,1,0,1}},
    // Preset 6
    {{1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},
     {0,1,1,0,0,0,1,1,0,1,1,0,0,1,0,1},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
     {0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0},
     {1,1,1,1,0,1,1,1,1,1,1,1,0,1,1,1}},
    // Preset 7
    {{1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,1},
     {0,0,1,0,0,1,0,0,0,0,1,1,0,0,1,0},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
     {0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0},
     {0,0,1,1,1,1,1,1,0,0,1,1,1,1,1,1}},
    // Preset 8
    {{1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,1},
     {0,1,0,1,0,0,0,1,0,0,0,1,0,0,1,1},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
     {0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,1},
     {1,1,1,0,0,1,1,1,1,1,1,0,0,1,1,1}},
    // Preset 9
    {{1,0,0,1,0,0,1,0,0,0,0,1,0,1,1,0},
     {0,0,1,0,0,1,0,0,0,0,1,0,0,1,0,1},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
     {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0},
     {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}},
    // Preset 10
    {{1,0,0,1,0,0,1,0,1,0,0,1,1,1,0,1},
     {0,1,0,0,0,0,0,1,0,1,1,0,0,0,1,1},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
     {0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0},
     {0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1}},
    // Preset 11
    {{1,0,0,1,0,0,1,0,0,0,1,0,0,1,1,1},
     {0,0,1,0,0,0,0,1,0,1,0,0,0,1,0,1},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,1,0,1,0,0,0,1,0,1,0,1,0},
     {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0},
     {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}},
    // Preset 12
    {{1,0,1,1,0,0,1,0,0,1,1,0,0,0,1,1},
     {0,1,0,0,0,1,0,0,0,1,0,1,0,1,0,0},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,1},
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
     {0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0},
     {1,0,1,0,1,1,1,1,1,1,1,0,0,1,0,1}},
    // Preset 13
    {{1,0,0,1,0,0,1,0,0,0,0,1,0,1,1,0},
     {0,1,0,0,0,1,0,0,1,1,0,1,0,0,0,0},
     {0,0,1,0,0,0,0,1,0,0,1,0,0,0,0,1},
     {1,0,1,0,1,0,1,1,1,0,1,0,1,0,1,1},
     {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0},
     {1,0,1,0,1,0,1,1,1,1,1,1,1,0,1,0}},
    // Preset 14
    {{1,0,1,0,0,0,0,0,0,0,0,1,0,0,1,1},
     {0,0,0,0,1,1,1,1,0,0,0,0,1,1,0,0},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
     {0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,1},
     {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}},
    // Preset 15
    {{1,0,1,0,0,0,1,0,0,0,0,0,0,1,1,1},
     {0,0,0,0,0,1,0,0,1,1,1,1,0,0,0,0},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {0,0,0,0,0,0,0,0,1,0,0,0,0,0,1,0},
     {1,1,1,0,1,1,1,1,1,1,1,0,1,1,1,1}},
    // Preset 16
    {{1,0,1,0,0,0,1,0,0,0,0,1,0,1,0,1},
     {0,1,0,0,0,0,0,1,1,0,1,0,1,0,0,0},
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
     {0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0},
     {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0}}
};

// ----- Parameter definitions -----
static const _NT_parameter parameters[] = {
    // RAND page (0-2)
    { .name="Randomise!", .min=0, .max=1,   .def=0,   .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, // IDX_RANDOMIZE  = 0
    { .name="RandomNESS", .min=0, .max=127, .def=0,   .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr  }, // IDX_RANDOMNESS = 1
    { .name="FREEZ!",     .min=0, .max=1,   .def=0,   .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, // IDX_FREEZ      = 2

    // ROUTING page (3-5)
    { .name="Clock input",    .min=1, .max=64, .def=21, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // IDX_CLOCK_INPUT = 3
    { .name="ShuffleClockIn", .min=0, .max=64, .def=0,  .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // IDX_SHUFFLE_CLK = 4
    { .name="RecClock",       .min=0, .max=64, .def=0,  .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // IDX_REC_CLOCK   = 5

    // PARAM page: Steps 1-16 (6-21)
    { .name="Steps 1",  .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, //  6
    { .name="Steps 2",  .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, //  7
    { .name="Steps 3",  .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, //  8
    { .name="Steps 4",  .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, //  9
    { .name="Steps 5",  .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 10
    { .name="Steps 6",  .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 11
    { .name="Steps 7",  .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 12
    { .name="Steps 8",  .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 13
    { .name="Steps 9",  .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 14
    { .name="Steps 10", .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 15
    { .name="Steps 11", .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 16
    { .name="Steps 12", .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 17
    { .name="Steps 13", .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 18
    { .name="Steps 14", .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 19
    { .name="Steps 15", .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 20
    { .name="Steps 16", .min=1, .max=16, .def=8, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 21

    // PARAM page: Range 1-16 (22-37)
    { .name="Range 1",  .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 22
    { .name="Range 2",  .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 23
    { .name="Range 3",  .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 24
    { .name="Range 4",  .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 25
    { .name="Range 5",  .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 26
    { .name="Range 6",  .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 27
    { .name="Range 7",  .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 28
    { .name="Range 8",  .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 29
    { .name="Range 9",  .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 30
    { .name="Range 10", .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 31
    { .name="Range 11", .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 32
    { .name="Range 12", .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 33
    { .name="Range 13", .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 34
    { .name="Range 14", .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 35
    { .name="Range 15", .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 36
    { .name="Range 16", .min=0, .max=127, .def=127, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 37

    // TRIGGERS page (38-47)
    { .name="Triggers RND", .min=0, .max=1,   .def=0,  .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, // IDX_TRIG_RND    = 38
    { .name="Threshold",    .min=1, .max=128, .def=90, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr  }, // IDX_TRIG_THRESH = 39
    { .name="Length 1", .min=1, .max=16, .def=16, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 40
    { .name="Length 2", .min=1, .max=16, .def=16, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 41
    { .name="Length 3", .min=1, .max=16, .def=16, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 42
    { .name="Length 4", .min=1, .max=16, .def=16, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 43
    { .name="Length 5", .min=1, .max=16, .def=16, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 44
    { .name="Length 6", .min=1, .max=16, .def=16, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 45
    { .name="Length 7", .min=1, .max=16, .def=16, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 46
    { .name="Length 8", .min=1, .max=16, .def=16, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr }, // 47

    // SEQUENCER page (48-55)
    { .name="Preset",     .min=1, .max=17,  .def=1, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr  }, // IDX_SEQ_PRESET     = 48
    { .name="Target",     .min=1, .max=17,  .def=1, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr  }, // IDX_SEQ_TARGET     = 49
    { .name="Crossfade",  .min=0, .max=127, .def=0, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr  }, // IDX_SEQ_CROSSFADE  = 50
    { .name="Start/Stop", .min=0, .max=1,   .def=1, .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, // IDX_SEQ_START_STOP = 51
    { .name="DRMlengths", .min=1, .max=8,   .def=1, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr  }, // IDX_DRM_LENGTHS    = 52
    { .name="MorphX",     .min=0, .max=127, .def=0, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr  }, // IDX_MORPH_X        = 53
    { .name="MorphY",     .min=0, .max=127, .def=0, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr  }, // IDX_MORPH_Y        = 54
    { .name="ZERO",       .min=0, .max=1,   .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, // IDX_ZERO           = 55

    // SHUFFLE page (56-63)
    { .name="Skick",  .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, // IDX_SHUFFLE_BASE + 0 = 56
    { .name="Ssub",   .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, //                    = 57
    { .name="Ssub2",  .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, //                    = 58
    { .name="Soh",    .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, //                    = 59
    { .name="Ssnare", .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, //                    = 60
    { .name="Sch",    .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, //                    = 61
    { .name="Sride",  .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, //                    = 62
    { .name="Sclap",  .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, //                    = 63

    // RECORDER page (64-66)
    { .name="Record",    .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, // IDX_RECORD     = 64
    { .name="RecLength", .min=1, .max=8, .def=4, .unit=kNT_unitNone, .scaling=0, .enumStrings=nullptr  }, // IDX_REC_LENGTH = 65
    { .name="Live!",     .min=0, .max=1, .def=0, .unit=kNT_unitEnum, .scaling=0, .enumStrings=enumOnOff }, // IDX_LIVE       = 66
};
// ARRAY_SIZE(parameters) == NUM_PARAMS == 67

// ----- Page parameter index arrays -----
static const uint8_t pageRandIdx[]    = { 0, 1, 2 };
static const uint8_t pageRoutingIdx[] = { 3, 4, 5 };
static const uint8_t pageParamIdx[]   = {
     6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,  // Steps 1-16
    22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37    // Range 1-16
};
static const uint8_t pageTrigIdx[]    = { 38,39,40,41,42,43,44,45,46,47 };
static const uint8_t pageSeqIdx[]     = { 48,49,50,51,52,53,54,55 };
static const uint8_t pageShuffleIdx[] = { 56,57,58,59,60,61,62,63 };
static const uint8_t pageRecIdx[]     = { 64,65,66 };

static const _NT_parameterPage pages[] = {
    { .name="RAND",     .numParams=ARRAY_SIZE(pageRandIdx),    .group=0, .params=pageRandIdx    },
    { .name="ROUTING",  .numParams=ARRAY_SIZE(pageRoutingIdx), .group=0, .params=pageRoutingIdx },
    { .name="PARAM",    .numParams=ARRAY_SIZE(pageParamIdx),   .group=0, .params=pageParamIdx   },
    { .name="TRIGGERS", .numParams=ARRAY_SIZE(pageTrigIdx),    .group=0, .params=pageTrigIdx    },
    { .name="SEQUENCER",.numParams=ARRAY_SIZE(pageSeqIdx),     .group=0, .params=pageSeqIdx     },
    { .name="SHUFFLE",  .numParams=ARRAY_SIZE(pageShuffleIdx), .group=0, .params=pageShuffleIdx },
    { .name="RECORDER", .numParams=ARRAY_SIZE(pageRecIdx),     .group=0, .params=pageRecIdx     },
};
static const _NT_parameterPages parameterPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages    = pages
};

// ----- Plugin state -----
struct Plugin : _NT_algorithm {
    // CC sequences
    uint8_t  ccVals[MAX_SEQS][MAX_STEPS][4];   // 4 morph variants per step
    uint8_t  pos[MAX_SEQS];                    // current step position per sequence
    uint8_t  steps[MAX_SEQS];                  // cached from v[IDX_STEPS_BASE+i]
    uint8_t  ranges[MAX_SEQS];                 // cached from v[IDX_RANGE_BASE+i]
    bool     randomize;
    uint8_t  randomness;
    bool     freez;
    uint8_t  frozenCcVals[MAX_SEQS];
    uint8_t  morphX, morphY;

    // Clock edge detection
    float    lastClock;
    float    lastShuffleClock;
    float    lastRecClock;

    // Cooldown counters — samples since last accepted rising edge.
    // Limits all clock inputs to max 20 Hz, preventing audio-rate inputs
    // from triggering excessive MIDI output or bus pulse storms.
    uint32_t clkCooldown;
    uint32_t shClkCooldown;
    uint32_t recClkCooldown;

    // Trigger sequences
    uint8_t  trigWeights[TRIG_SEQS][TRIG_STEPS];
    uint8_t  trigPos[TRIG_SEQS];
    uint8_t  trigLen[TRIG_SEQS];                // cached from v[IDX_TRIG_LEN_BASE+i]
    uint8_t  trigThresh;                        // cached from v[IDX_TRIG_THRESH]
    bool     trigRnd;
    uint8_t  trigZeroMask[TRIG_SEQS][TRIG_STEPS];

    // Step sequencer (8 drum tracks)
    uint8_t  seqPos[SEQ_TRACKS];
    uint8_t  seqLen[SEQ_TRACKS];               // set by DRMlengths preset
    uint8_t  seqCurrentPreset;                 // 0-15 or 16 (Pattern 17 = recording)
    uint8_t  seqTargetPreset;
    uint8_t  seqCrossfade;
    bool     seqStartStop;

    // Per-drum shuffle routing: false=main clock, true=shuffle clock
    bool     shuffleEnabled[SEQ_TRACKS];


    // Recorder
    RecMode  recorderMode;
    uint8_t  prevRecord;
    uint8_t  prevLive;
    uint32_t recEventCount;
    uint32_t recTotalSteps;
    uint32_t recCurrentStep;
    uint32_t recPlaybackStep;
    uint32_t recPlayIdx;
    uint32_t noteMask[4];   // 128-bit: which MIDI notes have been recorded
    uint32_t ccMask[4];     // 128-bit: which MIDI CCs have been recorded

    // Pointers into DRAM recorder buffer (set in construct())
    uint16_t* recStep;
    uint8_t*  recB0;
    uint8_t*  recB1;
    uint8_t*  recB2;
};

// ============================================================
// ----- Helpers -----
// ============================================================

static uint8_t getMorphedCC(Plugin* p, int track, int step) {
    float x = (float)p->morphX / 127.0f;
    float y = (float)p->morphY / 127.0f;
    float A = (float)p->ccVals[track][step][0];
    float B = (float)p->ccVals[track][step][1];
    float C = (float)p->ccVals[track][step][2];
    float D = (float)p->ccVals[track][step][3];
    float result = (A*(1.0f-x) + B*x)*(1.0f-y) + (C*(1.0f-x) + D*x)*y;
    if (result < 0.0f)   result = 0.0f;
    if (result > 127.0f) result = 127.0f;
    return (uint8_t)result;
}

static void genSequences(Plugin* p) {
    for (int i = 0; i < MAX_SEQS; ++i)
        for (int s = 0; s < p->steps[i]; ++s)
            for (int q = 0; q < 4; ++q)
                p->ccVals[i][s][q] = (uint8_t)(rand() % (p->ranges[i] + 1));
}

static void genTrig(Plugin* p) {
    for (int i = 0; i < TRIG_SEQS; ++i) {
        if (p->trigRnd) {
            for (int s = 0; s < TRIG_STEPS; ++s) {
                p->trigWeights[i][s]  = (uint8_t)((rand() % 128) + 1);
                p->trigZeroMask[i][s] = 1;
            }
        }
        p->trigPos[i] = 0;
    }
}

// Insertion sort — fast on nearly-sorted data; the recorder buffer is naturally
// almost sorted because events arrive in step order.
static void sortRecBuffer(Plugin* p) {
    uint32_t n = p->recEventCount;
    for (uint32_t i = 1; i < n; ++i) {
        uint16_t ks  = p->recStep[i];
        uint8_t  kb0 = p->recB0[i], kb1 = p->recB1[i], kb2 = p->recB2[i];
        int32_t  j   = (int32_t)i - 1;
        while (j >= 0 && p->recStep[j] > ks) {
            p->recStep[j+1] = p->recStep[j];
            p->recB0[j+1]   = p->recB0[j];
            p->recB1[j+1]   = p->recB1[j];
            p->recB2[j+1]   = p->recB2[j];
            --j;
        }
        p->recStep[j+1] = ks;
        p->recB0[j+1]   = kb0;
        p->recB1[j+1]   = kb1;
        p->recB2[j+1]   = kb2;
    }
}

static void clearRecorder(Plugin* p) {
    p->recEventCount   = 0;
    p->recCurrentStep  = 0;
    p->recPlaybackStep = 0;
    p->recPlayIdx      = 0;
    p->recorderMode    = REC_LIVE;
    for (int i = 0; i < 4; ++i) { p->noteMask[i] = 0; p->ccMask[i] = 0; }
}

// Send all recorded events at 'step'; advance recPlayIdx past them.
static void playbackStep(Plugin* p, uint32_t step, uint32_t midiDest, uint8_t probDrums)
{
    // Seek past events from earlier steps
    while (p->recPlayIdx < p->recEventCount &&
           p->recStep[p->recPlayIdx] < (uint16_t)step)
        p->recPlayIdx++;

    // Fire events for this step; CH10 drum notes use crossfade probability
    uint32_t idx = p->recPlayIdx;
    while (idx < p->recEventCount &&
           p->recStep[idx] == (uint16_t)step) {
        uint8_t b0 = p->recB0[idx], b1 = p->recB1[idx], b2 = p->recB2[idx];
        bool isDrum = ((b0 & 0xF0) == 0x90 && (b0 & 0x0F) == 9 && b2 > 0);
        if (!isDrum || (uint8_t)(rand() % 128) < probDrums)
            NT_sendMidi3ByteMessage(midiDest, b0, b1, b2);
        ++idx;
    }
    p->recPlayIdx = idx;
}

// Advance drum tracks that use the specified clock and fire MIDI NoteOn on ch10.
// shuffleOnly=false → main-clock tracks; shuffleOnly=true → shuffle-clock tracks.
static void fireDrumStep(Plugin* p, bool shuffleOnly, uint32_t midiDest) {
    if (!p->seqStartStop) return;

    uint8_t preset = p->seqCurrentPreset;  // 0-15 or 16 (Pattern 17)
    uint8_t target = p->seqTargetPreset;
    uint8_t cf     = p->seqCrossfade;

    for (int t = 0; t < SEQ_TRACKS; ++t) {
        if ((bool)p->shuffleEnabled[t] != shuffleOnly) continue;

        uint8_t pos = p->seqPos[t];
        uint8_t len = p->seqLen[t];
        p->seqPos[t] = (uint8_t)((pos + 1) % len);

        // Look up preset steps (tracks 0-5 from table; tracks 6-7 always 0)
        uint8_t presetStep = (preset < 16 && t < 6) ? SEQUENCE_PRESETS[preset][t][pos] : 0;
        uint8_t targetStep = (target < 16 && t < 6) ? SEQUENCE_PRESETS[target][t][pos] : 0;

        uint8_t stepToPlay = 0;

        if (preset == 16 && target == 16) {
            // Both sides are Pattern 17 (recording): nothing fires from the table
            continue;
        } else if (preset == 16) {
            // Source=recording (fires from RecClock); target=normal preset.
            // On main/shuffle clock only fire target at probability cf/127.
            if (cf > 0 && (uint8_t)(rand() % 128) < cf)
                stepToPlay = targetStep;
        } else if (target == 16) {
            // Source=normal preset; target=recording (fires from RecClock).
            // On main/shuffle clock fire source at probability (127-cf)/127.
            if (cf < 127 && (uint8_t)(rand() % 128) >= cf)
                stepToPlay = presetStep;
            // cf==127: source fully faded out, stepToPlay stays 0
        } else {
            // Normal crossfade between two presets
            if (cf == 0) {
                stepToPlay = presetStep;
            } else if (cf >= 127) {
                stepToPlay = targetStep;
            } else {
                stepToPlay = ((uint8_t)(rand() % 128) < cf) ? targetStep : presetStep;
            }
        }

        if (stepToPlay)
            NT_sendMidi3ByteMessage(midiDest, 0x99, (uint8_t)(36 + t), 127);
    }
}

static void fireCCSequences(Plugin* p, uint32_t midiDest) {
    for (int i = 0; i < MAX_SEQS; ++i) {
        uint8_t val;
        if (p->freez) {
            val = p->frozenCcVals[i];
        } else if (p->randomize) {
            val = getMorphedCC(p, i, p->pos[i]);
        } else {
            // Randomness: probabilistically replace current step values
            if (p->randomness > 0 && (uint8_t)(rand() % 128) < p->randomness)
                for (int q = 0; q < 4; ++q)
                    p->ccVals[i][p->pos[i]][q] = (uint8_t)(rand() % (p->ranges[i] + 1));
            // Clamp values to current range
            for (int q = 0; q < 4; ++q)
                if (p->ccVals[i][p->pos[i]][q] > p->ranges[i])
                    p->ccVals[i][p->pos[i]][q] = p->ranges[i];
            val = getMorphedCC(p, i, p->pos[i]);
            p->pos[i] = (uint8_t)((p->pos[i] + 1) % p->steps[i]);
        }
        // CC#17-32 on Channel 3 (0xB2)
        NT_sendMidi3ByteMessage(midiDest, 0xB0 | 2, (uint8_t)(17 + i), val);
    }
}

static void fireTrigSequences(Plugin* p, _NT_algorithm* alg, uint32_t midiDest) {
    uint8_t zeroOn = (alg->v[IDX_ZERO] > 0) ? 1 : 0;
    for (int i = 0; i < TRIG_SEQS; ++i) {
        uint8_t pos = (uint8_t)(p->trigPos[i] % p->trigLen[i]);
        if (zeroOn) p->trigZeroMask[i][pos] = 0;  // punch a silent hole
        if (p->trigWeights[i][pos] >= p->trigThresh && p->trigZeroMask[i][pos])
            NT_sendMidi3ByteMessage(midiDest, 0x90 | 1, (uint8_t)i, 127);
        p->trigPos[i] = (uint8_t)((pos + 1) % p->trigLen[i]);
    }
}

// ============================================================
// ----- Incoming MIDI handler -----
// ============================================================
extern "C" void midiMessageBZM5(_NT_algorithm* alg, uint8_t b0, uint8_t b1, uint8_t b2) {
    Plugin*  p        = (Plugin*)alg;
    const uint32_t midiDest   = kNT_destinationBreakout | kNT_destinationInternal;

    // CH10 notes 0-7 always trigger drums as MIDI NoteOn on ch10, regardless of recorder state
    if ((b0 & 0xF0) == 0x90 && (b0 & 0x0F) == 9 && b1 < 8 && b2 > 0)
        NT_sendMidi3ByteMessage(midiDest, 0x99, (uint8_t)(36 + b1), 127);

    switch (p->recorderMode) {
    case REC_LIVE:
        // Pass everything through
        NT_sendMidi3ByteMessage(midiDest, b0, b1, b2);
        break;

    case REC_RECORDING:
    case REC_OVERDUB:
        // Record into buffer (if not full), then pass through
        if (p->recEventCount < MAX_REC_EVENTS) {
            uint32_t idx       = p->recEventCount++;
            p->recStep[idx]    = (uint16_t)p->recCurrentStep;
            p->recB0[idx]      = b0;
            p->recB1[idx]      = b1;
            p->recB2[idx]      = b2;
            // Track note and CC masks for playback masking
            if ((b0 & 0xF0) == 0x90) {
                uint8_t n = b1 & 0x7F;
                p->noteMask[n >> 5] |= (1u << (n & 31));
            } else if ((b0 & 0xF0) == 0xB0) {
                uint8_t c = b1 & 0x7F;
                p->ccMask[c >> 5] |= (1u << (c & 31));
            }
        }
        NT_sendMidi3ByteMessage(midiDest, b0, b1, b2);
        break;

    case REC_PLAYBACK: {
        // Pass through only where not masked (loop takes over those lanes)
        uint8_t type = b0 & 0xF0;
        if (type == 0x90 || type == 0x80) {
            uint8_t n = b1 & 0x7F;
            if (!((p->noteMask[n >> 5] >> (n & 31)) & 1))
                NT_sendMidi3ByteMessage(midiDest, b0, b1, b2);
        } else if (type == 0xB0) {
            uint8_t c = b1 & 0x7F;
            if (!((p->ccMask[c >> 5] >> (c & 31)) & 1))
                NT_sendMidi3ByteMessage(midiDest, b0, b1, b2);
        } else {
            NT_sendMidi3ByteMessage(midiDest, b0, b1, b2);
        }
        break;
    }
    }
}

// ============================================================
// ----- Audio step -----
// ============================================================
extern "C" void pluginStepBZM5(_NT_algorithm* alg, float* busFrames, int numFramesBy4) {
    Plugin*    p         = (Plugin*)alg;
    const int  numFrames = numFramesBy4 * 4;

    const uint32_t midiDest   = kNT_destinationBreakout | kNT_destinationInternal;

    const int     mainClkBus = alg->v[IDX_CLOCK_INPUT];
    const int     shClkBus   = alg->v[IDX_SHUFFLE_CLK];
    const int     recClkBus  = alg->v[IDX_REC_CLOCK];
    const uint8_t recHigh    = (alg->v[IDX_RECORD] > 0) ? 1 : 0;
    const uint8_t liveHigh   = (alg->v[IDX_LIVE]   > 0) ? 1 : 0;

    // ----- State transitions (once per buffer) -----

    // Live! rising edge: clear recording, return to live pass-through
    if (liveHigh && !p->prevLive)
        clearRecorder(p);

    // Record going low while recording/overdubbing: finalise and switch to playback
    if (!recHigh && (p->recorderMode == REC_RECORDING ||
                     p->recorderMode == REC_OVERDUB)) {
        sortRecBuffer(p);
        p->recPlaybackStep = 0;
        p->recPlayIdx      = 0;
        p->recorderMode    = REC_PLAYBACK;
    }

    p->prevRecord = recHigh;
    p->prevLive   = liveHigh;

    // Crossfade probability for recording drum playback from RecClock
    uint8_t preset  = p->seqCurrentPreset;
    uint8_t target  = p->seqTargetPreset;
    uint8_t cf      = p->seqCrossfade;
    uint8_t probRec = 127;  // default: recording at 100% probability
    if      (preset == 16 && target != 16) probRec = (uint8_t)(127 - cf);
    else if (target == 16 && preset != 16) probRec = cf;

    // Minimum samples between accepted clock edges (limits all clocks to 20 Hz)
    const uint32_t minClkInterval = (uint32_t)(NT_globals.sampleRate / 20.0f);

    // ----- Per-sample loop (clock detection only) -----
    for (int frame = 0; frame < numFrames; ++frame) {

        // Increment cooldown counters, capped at the threshold
        if (p->clkCooldown    < minClkInterval) ++p->clkCooldown;
        if (p->shClkCooldown  < minClkInterval) ++p->shClkCooldown;
        if (p->recClkCooldown < minClkInterval) ++p->recClkCooldown;

        // ----- Main clock edge -----
        if (mainClkBus >= 1 && mainClkBus <= 64) {
            float clk = busFrames[(mainClkBus - 1) * numFrames + frame];
            if (clk >= 1.0f && p->lastClock < 0.5f && p->clkCooldown >= minClkInterval) {
                p->clkCooldown = 0;
                fireDrumStep(p, false, midiDest);
                fireCCSequences(p, midiDest);
                fireTrigSequences(p, alg, midiDest);
            }
            p->lastClock = (clk >= 1.0f) ? 1.0f : (clk < 0.3f ? 0.0f : p->lastClock);
        }

        // ----- Shuffle clock edge -----
        if (shClkBus >= 1 && shClkBus <= 64) {
            float clk = busFrames[(shClkBus - 1) * numFrames + frame];
            if (clk >= 1.0f && p->lastShuffleClock < 0.5f && p->shClkCooldown >= minClkInterval) {
                p->shClkCooldown = 0;
                fireDrumStep(p, true, midiDest);
            }
            p->lastShuffleClock = (clk >= 1.0f) ? 1.0f : (clk < 0.3f ? 0.0f : p->lastShuffleClock);
        }

        // ----- RecClock edge -----
        if (recClkBus >= 1 && recClkBus <= 64) {
            float clk = busFrames[(recClkBus - 1) * numFrames + frame];
            if (clk >= 1.0f && p->lastRecClock < 0.5f && p->recClkCooldown >= minClkInterval) {
                p->recClkCooldown = 0;

                switch (p->recorderMode) {
                case REC_LIVE:
                    if (recHigh) {
                        // Start recording on first RecClock tick after Record goes high
                        p->recEventCount   = 0;
                        p->recCurrentStep  = 0;
                        p->recPlaybackStep = 0;
                        p->recPlayIdx      = 0;
                        p->recTotalSteps   = (uint32_t)(alg->v[IDX_REC_LENGTH]) * 16u;
                        for (int i = 0; i < 4; ++i) { p->noteMask[i] = 0; p->ccMask[i] = 0; }
                        p->recorderMode    = REC_RECORDING;
                    }
                    break;

                case REC_RECORDING:
                    p->recCurrentStep++;
                    if (p->recCurrentStep >= p->recTotalSteps) {
                        // Loop length reached: switch to overdub
                        sortRecBuffer(p);
                        p->recCurrentStep  = 0;
                        p->recPlaybackStep = 0;
                        p->recPlayIdx      = 0;
                        p->recorderMode    = REC_OVERDUB;
                        playbackStep(p, 0, midiDest, probRec);
                    }
                    break;

                case REC_OVERDUB:
                    p->recCurrentStep = (p->recCurrentStep + 1) % p->recTotalSteps;
                    if (p->recCurrentStep == 0) {
                        sortRecBuffer(p);
                        p->recPlayIdx = 0;
                    }
                    p->recPlaybackStep = p->recCurrentStep;
                    playbackStep(p, p->recPlaybackStep, midiDest, probRec);
                    break;

                case REC_PLAYBACK:
                    p->recPlaybackStep = (p->recPlaybackStep + 1) % p->recTotalSteps;
                    if (p->recPlaybackStep == 0) p->recPlayIdx = 0;
                    playbackStep(p, p->recPlaybackStep, midiDest, probRec);
                    // Re-enter overdub if Record went high
                    if (recHigh) {
                        p->recCurrentStep = p->recPlaybackStep;
                        p->recorderMode   = REC_OVERDUB;
                    }
                    break;
                }
            }
            p->lastRecClock = (clk >= 1.0f) ? 1.0f : (clk < 0.3f ? 0.0f : p->lastRecClock);
        }
    }

}

// ============================================================
// ----- Parameter changed -----
// ============================================================
extern "C" void parameterChangedBZM5(_NT_algorithm* alg, int idx) {
    Plugin* p = (Plugin*)alg;

    if (idx == IDX_RANDOMIZE) {
        bool newRand = (alg->v[idx] > 0);
        if (!newRand && p->randomize) genSequences(p);  // 1→0 transition = generate new sequences
        p->randomize = newRand;
    }
    else if (idx == IDX_RANDOMNESS) {
        p->randomness = (uint8_t)alg->v[idx];
    }
    else if (idx == IDX_FREEZ) {
        bool nf = (alg->v[idx] > 0);
        if (nf && !p->freez)
            for (int i = 0; i < MAX_SEQS; ++i)
                p->frozenCcVals[i] = getMorphedCC(p, i, p->pos[i]);
        p->freez = nf;
    }
    else if (idx == IDX_CLOCK_INPUT) {
        p->lastClock = 0.0f;
    }
    else if (idx == IDX_SHUFFLE_CLK) {
        p->lastShuffleClock = 0.0f;
    }
    else if (idx == IDX_REC_CLOCK) {
        p->lastRecClock = 0.0f;
    }
    else if (idx >= IDX_STEPS_BASE && idx < IDX_STEPS_BASE + MAX_SEQS) {
        int seq = idx - IDX_STEPS_BASE;
        p->steps[seq] = (uint8_t)alg->v[idx];
        if (p->randomize)
            for (int s = 0; s < p->steps[seq]; ++s)
                for (int q = 0; q < 4; ++q)
                    p->ccVals[seq][s][q] = (uint8_t)(rand() % (p->ranges[seq] + 1));
    }
    else if (idx >= IDX_RANGE_BASE && idx < IDX_RANGE_BASE + MAX_SEQS) {
        int seq = idx - IDX_RANGE_BASE;
        p->ranges[seq] = (uint8_t)alg->v[idx];
        if (p->randomize)
            for (int s = 0; s < p->steps[seq]; ++s)
                for (int q = 0; q < 4; ++q)
                    p->ccVals[seq][s][q] = (uint8_t)(rand() % (p->ranges[seq] + 1));
    }
    else if (idx == IDX_TRIG_RND) {
        p->trigRnd = (alg->v[idx] > 0);
        if (p->trigRnd) genTrig(p);
    }
    else if (idx == IDX_TRIG_THRESH) {
        p->trigThresh = (uint8_t)alg->v[idx];
    }
    else if (idx >= IDX_TRIG_LEN_BASE && idx < IDX_TRIG_LEN_BASE + TRIG_SEQS) {
        p->trigLen[idx - IDX_TRIG_LEN_BASE] = (uint8_t)alg->v[idx];
    }
    else if (idx == IDX_SEQ_PRESET) {
        p->seqCurrentPreset = (uint8_t)(alg->v[idx] - 1);  // GUI 1-17 → internal 0-16
    }
    else if (idx == IDX_SEQ_TARGET) {
        p->seqTargetPreset = (uint8_t)(alg->v[idx] - 1);
    }
    else if (idx == IDX_SEQ_CROSSFADE) {
        p->seqCrossfade = (uint8_t)alg->v[idx];
    }
    else if (idx == IDX_SEQ_START_STOP) {
        bool ns = (alg->v[idx] > 0);
        if (ns && !p->seqStartStop)
            for (int t = 0; t < SEQ_TRACKS; ++t) p->seqPos[t] = 0;
        p->seqStartStop = ns;
    }
    else if (idx == IDX_DRM_LENGTHS) {
        uint8_t pr = (uint8_t)(alg->v[idx] - 1);  // GUI 1-8 → internal 0-7
        if (pr < 8)
            for (int i = 0; i < SEQ_TRACKS; ++i)
                p->seqLen[i] = DRUM_LENGTH_PRESETS[pr][i];
    }
    else if (idx == IDX_MORPH_X) {
        p->morphX = (uint8_t)alg->v[idx];
    }
    else if (idx == IDX_MORPH_Y) {
        p->morphY = (uint8_t)alg->v[idx];
    }
    else if (idx >= IDX_SHUFFLE_BASE && idx < IDX_SHUFFLE_BASE + SEQ_TRACKS) {
        p->shuffleEnabled[idx - IDX_SHUFFLE_BASE] = (alg->v[idx] > 0);
    }
    // IDX_ZERO: read directly from v[] in fireTrigSequences — no caching needed
    // IDX_RECORD, IDX_REC_LENGTH, IDX_LIVE: read directly from v[] in step()
}

// ============================================================
// ----- Requirements -----
// ============================================================
extern "C" void calculateRequirementsBZM5(_NT_algorithmRequirements& r, const int32_t*) {
    r.numParameters = ARRAY_SIZE(parameters);  // must equal NUM_PARAMS = 67
    r.sram = sizeof(Plugin);
    // DRAM: recorder buffer
    //   uint16_t recStep[2048] = 4096 bytes
    //   uint8_t  recB0[2048]   = 2048 bytes
    //   uint8_t  recB1[2048]   = 2048 bytes
    //   uint8_t  recB2[2048]   = 2048 bytes
    //   Total                  = 10240 bytes
    r.dram = (uint32_t)(MAX_REC_EVENTS * 2 + MAX_REC_EVENTS * 3);
    r.dtc  = 0;
    r.itc  = 0;
}

// ============================================================
// ----- Construct -----
// ============================================================
extern "C" _NT_algorithm* constructBZM5(const _NT_algorithmMemoryPtrs& ptrs,
                                         const _NT_algorithmRequirements&,
                                         const int32_t*)
{
    Plugin* p = new(ptrs.sram) Plugin();

    // Wire up DRAM recorder buffer layout
    uint8_t* dram = (uint8_t*)ptrs.dram;
    p->recStep = (uint16_t*)dram;
    p->recB0   = dram + MAX_REC_EVENTS * 2;
    p->recB1   = dram + MAX_REC_EVENTS * 2 + MAX_REC_EVENTS;
    p->recB2   = dram + MAX_REC_EVENTS * 2 + MAX_REC_EVENTS * 2;

    // CC sequences — init to match parameter .def values
    for (int i = 0; i < MAX_SEQS; ++i) {
        p->pos[i]          = 0;
        p->steps[i]        = 8;    // Steps .def = 8
        p->ranges[i]       = 127;  // Range .def = 127
        p->frozenCcVals[i] = 0;
    }
    p->randomize  = false;  // IDX_RANDOMIZE  .def = 0
    p->randomness = 0;      // IDX_RANDOMNESS .def = 0
    p->freez      = false;  // IDX_FREEZ      .def = 0
    p->morphX     = 0;      // IDX_MORPH_X    .def = 0
    p->morphY     = 0;      // IDX_MORPH_Y    .def = 0

    // Clock edge detectors
    p->lastClock        = 0.0f;
    p->lastShuffleClock = 0.0f;
    p->lastRecClock     = 0.0f;

    // Init cooldowns to the threshold so the first clock edge is accepted immediately
    const uint32_t initCooldown = (uint32_t)(NT_globals.sampleRate / 20.0f);
    p->clkCooldown    = initCooldown;
    p->shClkCooldown  = initCooldown;
    p->recClkCooldown = initCooldown;

    // Trigger sequences
    for (int i = 0; i < TRIG_SEQS; ++i) {
        p->trigPos[i] = 0;
        p->trigLen[i] = 16;  // Length .def = 16
        for (int s = 0; s < TRIG_STEPS; ++s) {
            p->trigWeights[i][s]  = (uint8_t)((rand() % 128) + 1);
            p->trigZeroMask[i][s] = 1;
        }
    }
    p->trigThresh = 90;    // Threshold .def = 90
    p->trigRnd    = false; // Triggers RND .def = 0

    // Step sequencer
    for (int t = 0; t < SEQ_TRACKS; ++t) {
        p->seqPos[t]         = 0;
        p->seqLen[t]         = 16;  // DRMlengths preset 1 = all 16
        p->shuffleEnabled[t] = false;
    }
    p->seqCurrentPreset = 0;    // Preset .def = 1 (GUI) → internal 0
    p->seqTargetPreset  = 0;    // Target .def = 1 (GUI) → internal 0
    p->seqCrossfade     = 0;    // Crossfade .def = 0
    p->seqStartStop     = true; // Start/Stop .def = 1 (ON)

    // Recorder initial state
    clearRecorder(p);
    p->prevRecord    = 0;
    p->prevLive      = 0;
    p->recTotalSteps = 64;  // RecLength .def = 4 bars × 16 steps/bar

    // Wire up plugin parameter tables
    p->parameters     = parameters;
    p->parameterPages = &parameterPages;

    // Populate initial CC sequence data
    genSequences(p);

    return (_NT_algorithm*)p;
}

// ============================================================
// ----- Plugin entry point -----
// ============================================================
extern "C" uintptr_t pluginEntry(_NT_selector s, uint32_t) {
    switch (s) {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo: {
        static const _NT_factory f = {
            .guid                        = NT_MULTICHAR('B','Z','M','5'),
            .name                        = "BizmuthSeq",
            .description                 = "Drum+CC sequencer with recorder",
            .numSpecifications           = 0,
            .specifications              = nullptr,
            .calculateStaticRequirements = nullptr,
            .initialise                  = nullptr,
            .calculateRequirements       = calculateRequirementsBZM5,
            .construct                   = constructBZM5,
            .parameterChanged            = parameterChangedBZM5,
            .step                        = pluginStepBZM5,
            .draw                        = nullptr,
            .midiRealtime                = nullptr,
            .midiMessage                 = midiMessageBZM5,
            .tags                        = kNT_tagUtility,
            .hasCustomUi                 = nullptr,
            .customUi                    = nullptr,
            .setupUi                     = nullptr,
            .serialise                   = nullptr,
            .deserialise                 = nullptr,
            .midiSysEx                   = nullptr,
            .parameterUiPrefix           = nullptr,
            .parameterString             = nullptr,
        };
        return (uintptr_t)&f;
    }
    default: return 0;
    }
}
