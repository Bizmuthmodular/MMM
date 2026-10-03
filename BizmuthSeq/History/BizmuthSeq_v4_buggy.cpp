// BizmuthSeq v4
// Changes from v3:
//   - Removed 8x clock system (1 step per rising edge)
//   - Removed offset page, burst generator, delta system
//   - Drums now output as 5ms bus pulses (buses 37-44) not MIDI
//   - All MIDI: DIN + Internal simultaneously
//   - Removed Include 1-16, Div 1-16, Dir 1-16 params
//   - Moved DRMlengths, MorphX, MorphY, ZERO to SEQUENCER page
//   - Added ShuffleClockIn, RecClock to ROUTING page
//   - Added SHUFFLE page (per-drum clock source selection)
//   - Added RECORDER page (Record, RecLength, Live!)
//   - Extended Preset/Target to 17 (pattern 17 = recording)
//   - Ch10 notes 0-7 → drum bus triggers
//   - DRMlengths extended to 8 tracks (7-8 = 16 all presets)
//   - Tracks 7-8 patterns cleared to empty (to be filled later)

#include "distingnt/api.h"
#include <new>
#include <cstdlib>
#include <cstring>

#define MAX_SEQS    16
#define MAX_STEPS   16
#define TRIG_SEQS    8
#define TRIG_STEPS  16
#define SEQ_TRACKS   8
#define NUM_PRESETS 16
#define MAX_REC_EVENTS 2048

// ----- Recorder modes -----
enum RecMode : uint8_t { REC_LIVE=0, REC_RECORDING=1, REC_OVERDUB=2, REC_PLAYBACK=3 };

// ----- Parameter indices -----
enum {
    // RAND page
    IDX_RANDOMIZE    = 0,
    IDX_RANDOMNESS,          // 1
    IDX_FREEZ,               // 2

    // ROUTING page
    IDX_CLOCK_INPUT,         // 3
    IDX_SHUFFLE_CLK,         // 4
    IDX_REC_CLOCK,           // 5

    // PARAM page: Steps 1-16 then Range 1-16
    IDX_STEPS_BASE   = 6,    // 6-21
    IDX_RANGE_BASE   = 22,   // 22-37

    // TRIGGERS page
    IDX_TRIG_RND     = 38,
    IDX_TRIG_THRESH,         // 39
    IDX_TRIG_LEN_BASE,       // 40-47

    // SEQUENCER page
    IDX_SEQ_PRESET   = 48,
    IDX_SEQ_TARGET,          // 49
    IDX_SEQ_CROSSFADE,       // 50
    IDX_SEQ_START_STOP,      // 51
    IDX_DRM_LENGTHS,         // 52
    IDX_MORPH_X,             // 53
    IDX_MORPH_Y,             // 54
    IDX_ZERO,                // 55

    // SHUFFLE page (per-drum clock selection)
    IDX_SHUFFLE_BASE = 56,   // 56-63: Skick,Ssub,Ssub2,Soh,Ssnare,Sch,Sride,Sclap

    // RECORDER page
    IDX_RECORD       = 64,
    IDX_REC_LENGTH,          // 65
    IDX_LIVE,                // 66

    NUM_PARAMS       = 67
};

// ----- UI enums -----
static const char* enumOnOff[] = { "OFF", "ON", nullptr };

// ----- Drum Length Presets (8 presets × 8 tracks) -----
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

// ----- Sequence Presets (16 presets, tracks 1-6 only) -----
// Track order: Kick(0), Sub(1), Sub2(2->maps to OH), OH(3->maps to Ride), Snare(4), CH(5)
// NOTE: Original preset layout was Kick/Sub/OH/Ride/Snare/CH (indices 0-5)
// New bus order: kick=37, sub=38, sub2=39, oh=40, snare=41, ch=42, ride=43, clap=44
// Presets map track index 0-5 directly to drum voices 0-5 (kick/sub/sub2/oh/snare/ch)
static const uint8_t SEQUENCE_PRESETS[NUM_PRESETS][6][16] = {
    // Preset 1
    {{1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},  // Kick
     {0,0,1,0,0,1,0,0,0,0,0,1,0,1,0,0},  // Sub
     {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},  // Sub2 (was OH)
     {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},  // OH (was Ride)
     {0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0},  // Snare
     {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}}, // CH

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
static const _NT_parameter parameters[NUM_PARAMS] = {
    // RAND page (0-2)
    {.name="Randomise!", .min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},
    {.name="RandomNESS", .min=0,.max=127,.def=0,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="FREEZ!",     .min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},

    // ROUTING page (3-5)
    {.name="Clock input",    .min=1,.max=64,.def=21,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="ShuffleClockIn",.min=0,.max=64,.def=0, .unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="RecClock",       .min=0,.max=64,.def=0, .unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},

    // PARAM page: Steps 1-16 (6-21)
    {.name="Steps 1", .min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 2", .min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 3", .min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 4", .min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 5", .min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 6", .min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 7", .min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 8", .min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 9", .min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 10",.min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 11",.min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 12",.min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 13",.min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 14",.min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 15",.min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Steps 16",.min=1,.max=16,.def=8,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},

    // PARAM page: Range 1-16 (22-37)
    {.name="Range 1", .min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 2", .min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 3", .min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 4", .min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 5", .min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 6", .min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 7", .min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 8", .min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 9", .min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 10",.min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 11",.min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 12",.min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 13",.min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 14",.min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 15",.min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Range 16",.min=0,.max=127,.def=127,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},

    // TRIGGERS page (38-47)
    {.name="Triggers RND",.min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},
    {.name="Threshold",   .min=1,.max=128,.def=90,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Length 1",.min=1,.max=16,.def=16,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Length 2",.min=1,.max=16,.def=16,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Length 3",.min=1,.max=16,.def=16,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Length 4",.min=1,.max=16,.def=16,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Length 5",.min=1,.max=16,.def=16,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Length 6",.min=1,.max=16,.def=16,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Length 7",.min=1,.max=16,.def=16,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Length 8",.min=1,.max=16,.def=16,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},

    // SEQUENCER page (48-55)
    {.name="Preset",    .min=1,.max=17,.def=1,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Target",    .min=1,.max=17,.def=1,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Crossfade", .min=0,.max=127,.def=0,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Start/Stop",.min=0,.max=1,.def=1,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},
    {.name="DRMlengths",.min=1,.max=8,.def=1,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="MorphX",    .min=0,.max=127,.def=0,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="MorphY",    .min=0,.max=127,.def=0,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="ZERO",      .min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},

    // SHUFFLE page (56-63)
    {.name="Skick", .min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},
    {.name="Ssub",  .min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},
    {.name="Ssub2", .min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},
    {.name="Soh",   .min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},
    {.name="Ssnare",.min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},
    {.name="Sch",   .min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},
    {.name="Sride", .min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},
    {.name="Sclap", .min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},

    // RECORDER page (64-66)
    {.name="Record",    .min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},
    {.name="RecLength", .min=1,.max=8,.def=4,.unit=kNT_unitNone,.scaling=0,.enumStrings=nullptr},
    {.name="Live!",     .min=0,.max=1,.def=0,.unit=kNT_unitEnum,.scaling=0,.enumStrings=enumOnOff},
};

// ----- Page parameter index arrays -----
static const uint8_t pageRandIdx[]    = { 0, 1, 2 };
static const uint8_t pageRoutingIdx[] = { 3, 4, 5 };
static const uint8_t pageParamIdx[]   = {
     6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,  // Steps 1-16
    22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37   // Range 1-16
};
static const uint8_t pageTrigIdx[]    = { 38,39,40,41,42,43,44,45,46,47 };
static const uint8_t pageSeqIdx[]     = { 48,49,50,51,52,53,54,55 };
static const uint8_t pageShuffleIdx[] = { 56,57,58,59,60,61,62,63 };
static const uint8_t pageRecIdx[]     = { 64,65,66 };

static const _NT_parameterPage pages[] = {
    {.name="RAND",     .numParams=ARRAY_SIZE(pageRandIdx),    .group=0,.params=pageRandIdx},
    {.name="ROUTING",  .numParams=ARRAY_SIZE(pageRoutingIdx), .group=0,.params=pageRoutingIdx},
    {.name="PARAM",    .numParams=ARRAY_SIZE(pageParamIdx),   .group=0,.params=pageParamIdx},
    {.name="TRIGGERS", .numParams=ARRAY_SIZE(pageTrigIdx),    .group=0,.params=pageTrigIdx},
    {.name="SEQUENCER",.numParams=ARRAY_SIZE(pageSeqIdx),     .group=0,.params=pageSeqIdx},
    {.name="SHUFFLE",  .numParams=ARRAY_SIZE(pageShuffleIdx), .group=0,.params=pageShuffleIdx},
    {.name="RECORDER", .numParams=ARRAY_SIZE(pageRecIdx),     .group=0,.params=pageRecIdx},
};
static const _NT_parameterPages parameterPages = { .numPages=ARRAY_SIZE(pages), .pages=pages };

// ----- Plugin state -----
struct Plugin : _NT_algorithm {
    // CC sequences state
    uint8_t ccVals[MAX_SEQS][MAX_STEPS][4];   // 4 morph variants per step
    uint8_t pos[MAX_SEQS];
    uint8_t steps[MAX_SEQS];
    uint8_t ranges[MAX_SEQS];
    bool    randomize;
    uint8_t randomness;
    bool    freez;
    uint8_t frozenCcVals[MAX_SEQS];
    uint8_t morphX, morphY;

    // Clock edge state
    float    lastClock;
    float    lastShuffleClock;
    float    lastRecClock;
    // Cooldown counters: samples since last accepted rising edge.
    // Limits all clock inputs to max 300 BPM 1/16 (20 Hz = sampleRate/20 samples).
    // Prevents audio-rate signals on accidentally-selected buses from freezing the plugin.
    uint32_t clkCooldown;
    uint32_t shClkCooldown;
    uint32_t recClkCooldown;

    // Trigger sequences
    uint8_t trigWeights[TRIG_SEQS][TRIG_STEPS];
    uint8_t trigPos[TRIG_SEQS];
    uint8_t trigLen[TRIG_SEQS];
    uint8_t trigThresh;
    bool    trigRnd;
    uint8_t trigZeroMask[TRIG_SEQS][TRIG_STEPS];

    // Step sequencer (8 drum tracks)
    uint8_t seqPos[SEQ_TRACKS];
    uint8_t seqLen[SEQ_TRACKS];
    uint8_t seqCurrentPreset;   // 0-15 or 16 for pattern 17
    uint8_t seqTargetPreset;
    uint8_t seqCrossfade;
    bool    seqStartStop;

    // Shuffle routing (per drum track)
    bool    shuffleEnabled[SEQ_TRACKS];

    // Drum bus pulse counters (5ms = ~240 samples at 48kHz)
    uint16_t drumPulseCounters[SEQ_TRACKS];

    // Recorder
    RecMode  recorderMode;
    uint8_t  prevRecord;
    uint8_t  prevLive;
    uint32_t recEventCount;
    uint32_t recTotalSteps;
    uint32_t recCurrentStep;
    uint32_t recPlaybackStep;
    uint32_t recPlayIdx;
    uint32_t noteMask[4];   // 128-bit: which notes are recorded
    uint32_t ccMask[4];     // 128-bit: which CCs are recorded

    // DRAM pointers (recorder buffer)
    uint16_t* recStep;
    uint8_t*  recB0;
    uint8_t*  recB1;
    uint8_t*  recB2;
};

// ----- Helpers -----

static void genSequences(Plugin* p) {
    for (int i = 0; i < MAX_SEQS; ++i) {
        for (int s = 0; s < p->steps[i]; ++s)
            for (int q = 0; q < 4; ++q)
                p->ccVals[i][s][q] = rand() % (p->ranges[i] + 1);
    }
}

static uint8_t getMorphedCC(Plugin* p, int track, int step) {
    float x = (float)p->morphX / 127.0f;
    float y = (float)p->morphY / 127.0f;
    float A = p->ccVals[track][step][0];
    float B = p->ccVals[track][step][1];
    float C = p->ccVals[track][step][2];
    float D = p->ccVals[track][step][3];
    float result = (A*(1.0f-x) + B*x)*(1.0f-y) + (C*(1.0f-x) + D*x)*y;
    if (result < 0.0f) result = 0.0f;
    if (result > 127.0f) result = 127.0f;
    return (uint8_t)result;
}

static void genTrig(Plugin* p) {
    for (int i = 0; i < TRIG_SEQS; ++i) {
        if (p->trigRnd) {
            for (int s = 0; s < TRIG_STEPS; ++s) {
                p->trigWeights[i][s] = (rand() % 128) + 1;
                p->trigZeroMask[i][s] = 1;
            }
        }
        p->trigPos[i] = 0;
    }
}


static void sortRecBuffer(Plugin* p) {
    // Insertion sort by step (fast on nearly-sorted data; buffer is naturally nearly sorted)
    uint32_t n = p->recEventCount;
    for (uint32_t i = 1; i < n; ++i) {
        uint16_t ks = p->recStep[i];
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
    p->recEventCount    = 0;
    p->recCurrentStep   = 0;
    p->recPlaybackStep  = 0;
    p->recPlayIdx       = 0;
    p->recorderMode     = REC_LIVE;
    for (int i = 0; i < 4; ++i) { p->noteMask[i] = 0; p->ccMask[i] = 0; }
}

// Play back all recorded events at the given step; update recPlayIdx
static void playbackStep(Plugin* p, uint32_t step, uint32_t midiDest,
                         uint16_t pulseSamps, uint8_t probDrums)
{
    // Skip past events from earlier steps
    while (p->recPlayIdx < p->recEventCount &&
           p->recStep[p->recPlayIdx] < (uint16_t)step) {
        p->recPlayIdx++;
    }
    // Play events for this step
    uint32_t idx = p->recPlayIdx;
    while (idx < p->recEventCount &&
           p->recStep[idx] == (uint16_t)step) {
        uint8_t b0 = p->recB0[idx], b1 = p->recB1[idx], b2 = p->recB2[idx];
        // Send non-drum MIDI always
        NT_sendMidi3ByteMessage(midiDest, b0, b1, b2);
        // Ch10 note 0-7: also trigger drum bus (with crossfade probability)
        if ((b0 & 0xF0) == 0x90 && (b0 & 0x0F) == 9 && b1 < 8 && b2 > 0) {
            uint8_t rnd = (uint8_t)(rand() % 128);
            if (rnd < probDrums) {
                p->drumPulseCounters[b1] = pulseSamps;
            }
        }
        idx++;
    }
    p->recPlayIdx = idx;
}

// Fire drums for all tracks that use the specified clock source (shuffleOnly=false=main)
static void fireDrumStep(Plugin* p, bool shuffleOnly, uint16_t pulseSamps)
{
    if (!p->seqStartStop) return;

    uint8_t preset = p->seqCurrentPreset;  // 0-15 or 16
    uint8_t target = p->seqTargetPreset;
    uint8_t cf     = p->seqCrossfade;

    for (int t = 0; t < SEQ_TRACKS; ++t) {
        if ((bool)p->shuffleEnabled[t] != shuffleOnly) continue;

        uint8_t pos = p->seqPos[t];
        uint8_t len = p->seqLen[t];
        p->seqPos[t] = (pos + 1) % len;

        uint8_t presetStep = 0, targetStep = 0;

        // Get step from preset (tracks 0-5 from table, 6-7 always 0)
        if (preset < 16 && t < 6)
            presetStep = SEQUENCE_PRESETS[preset][t][pos];
        // preset==16: recording, handled via RecClock events, no step from table

        if (target < 16 && t < 6)
            targetStep = SEQUENCE_PRESETS[target][t][pos];

        uint8_t stepToPlay = 0;

        if (preset == 16 && target == 16) {
            // Both sides are recording - nothing from table clock
            continue;
        } else if (preset == 16) {
            // Source = recording (fires from RecClock), target = normal preset
            // On main/shuffle clock: only fire target with probability cf/127
            if (cf > 0) {
                uint8_t rnd = (uint8_t)(rand() % 128);
                if (rnd < cf) stepToPlay = targetStep;
            }
        } else if (target == 16) {
            // Source = normal preset, target = recording (fires from RecClock)
            // On main/shuffle clock: fire source with probability (127-cf)/127
            if (cf < 127) {
                uint8_t rnd = (uint8_t)(rand() % 128);
                if (rnd >= cf) stepToPlay = presetStep;
            } else {
                // cf==127: source fully faded out
                stepToPlay = 0;
            }
        } else {
            // Normal crossfade between two presets
            if (cf == 0) {
                stepToPlay = presetStep;
            } else if (cf >= 127) {
                stepToPlay = targetStep;
            } else {
                uint8_t rnd = (uint8_t)(rand() % 128);
                stepToPlay = (rnd < cf) ? targetStep : presetStep;
            }
        }

        if (stepToPlay) {
            p->drumPulseCounters[t] = pulseSamps;
        }
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
            if (p->randomness > 0) {
                uint8_t rc = (uint8_t)(rand() % 128);
                if (rc < p->randomness) {
                    for (int q = 0; q < 4; ++q)
                        p->ccVals[i][p->pos[i]][q] = rand() % (p->ranges[i] + 1);
                }
            }
            for (int q = 0; q < 4; ++q)
                if (p->ccVals[i][p->pos[i]][q] > p->ranges[i])
                    p->ccVals[i][p->pos[i]][q] = p->ranges[i];

            val = getMorphedCC(p, i, p->pos[i]);
            p->pos[i] = (p->pos[i] + 1) % p->steps[i];
        }
        NT_sendMidi3ByteMessage(midiDest, 0xB0 | 2, (uint8_t)(17 + i), val);
    }
}

static void fireTrigSequences(Plugin* p, _NT_algorithm* alg, uint32_t midiDest) {
    uint8_t zeroOn = (uint8_t)(alg->v[IDX_ZERO] > 0);
    for (int i = 0; i < TRIG_SEQS; ++i) {
        uint8_t pos = p->trigPos[i] % p->trigLen[i];
        if (zeroOn) p->trigZeroMask[i][pos] = 0;
        if (p->trigWeights[i][pos] >= p->trigThresh && p->trigZeroMask[i][pos]) {
            NT_sendMidi3ByteMessage(midiDest, 0x90 | 1, (uint8_t)i, 127);
        }
        p->trigPos[i] = (pos + 1) % p->trigLen[i];
    }
}

// ----- MIDI message callback (incoming DIN MIDI) -----
static void midiMessage(_NT_algorithm* alg, uint8_t b0, uint8_t b1, uint8_t b2) {
    Plugin* p = (Plugin*)alg;

    const uint32_t midiDest = kNT_destinationBreakout | kNT_destinationInternal;
    const uint16_t pulseSamps = (uint16_t)(NT_globals.sampleRate * 0.005f);

    // Ch10 notes 0-7 always trigger drum buses
    if ((b0 & 0xF0) == 0x90 && (b0 & 0x0F) == 9 && b1 < 8 && b2 > 0) {
        p->drumPulseCounters[b1] = pulseSamps;
    }

    switch (p->recorderMode) {
    case REC_LIVE:
        // Pass through all incoming MIDI
        NT_sendMidi3ByteMessage(midiDest, b0, b1, b2);
        break;

    case REC_RECORDING:
    case REC_OVERDUB:
        // Record event if buffer not full
        if (p->recEventCount < MAX_REC_EVENTS) {
            uint32_t idx = p->recEventCount++;
            p->recStep[idx] = (uint16_t)p->recCurrentStep;
            p->recB0[idx]   = b0;
            p->recB1[idx]   = b1;
            p->recB2[idx]   = b2;
            // Track what types are recorded (for masking)
            if ((b0 & 0xF0) == 0x90) {
                uint8_t n = b1 & 0x7F;
                p->noteMask[n >> 5] |= (1u << (n & 31));
            } else if ((b0 & 0xF0) == 0xB0) {
                uint8_t c = b1 & 0x7F;
                p->ccMask[c >> 5] |= (1u << (c & 31));
            }
        }
        // Pass through
        NT_sendMidi3ByteMessage(midiDest, b0, b1, b2);
        break;

    case REC_PLAYBACK: {
        // Pass through only if not masked
        uint8_t type = b0 & 0xF0;
        if (type == 0x90 || type == 0x80) {
            uint8_t n = b1 & 0x7F;
            bool masked = (p->noteMask[n >> 5] >> (n & 31)) & 1;
            if (!masked) NT_sendMidi3ByteMessage(midiDest, b0, b1, b2);
        } else if (type == 0xB0) {
            uint8_t c = b1 & 0x7F;
            bool masked = (p->ccMask[c >> 5] >> (c & 31)) & 1;
            if (!masked) NT_sendMidi3ByteMessage(midiDest, b0, b1, b2);
        } else {
            NT_sendMidi3ByteMessage(midiDest, b0, b1, b2);
        }
        break;
    }
    }
}

// ----- Audio step -----
static void pluginStep(_NT_algorithm* alg, float* busFrames, int numFramesBy4) {
    Plugin* p = (Plugin*)alg;
    const int numFrames = numFramesBy4 * 4;

    const uint32_t midiDest   = kNT_destinationBreakout | kNT_destinationInternal;
    const uint16_t pulseSamps = (uint16_t)(NT_globals.sampleRate * 0.005f);

    const int mainClkBus    = alg->v[IDX_CLOCK_INPUT];
    const int shClkBus      = alg->v[IDX_SHUFFLE_CLK];
    const int recClkBus     = alg->v[IDX_REC_CLOCK];
    const uint8_t recHigh   = (alg->v[IDX_RECORD] > 0) ? 1 : 0;
    const uint8_t liveHigh  = (alg->v[IDX_LIVE] > 0) ? 1 : 0;

    // --- State transitions (once per buffer) ---

    // Live! rising edge: clear recording, return to live
    if (liveHigh && !p->prevLive) {
        clearRecorder(p);
    }

    // Record going low: transition to playback
    if (!recHigh && (p->recorderMode == REC_RECORDING || p->recorderMode == REC_OVERDUB)) {
        sortRecBuffer(p);
        p->recPlaybackStep = 0;
        p->recPlayIdx      = 0;
        p->recorderMode    = REC_PLAYBACK;
    }

    p->prevRecord = recHigh;
    p->prevLive   = liveHigh;

    // Crossfade probability for recording drums (used in RecClock handler)
    uint8_t preset = p->seqCurrentPreset;
    uint8_t target = p->seqTargetPreset;
    uint8_t cf     = p->seqCrossfade;
    uint8_t probRec = 127; // default: recording at 100%
    if (preset == 16 && target != 16) {
        probRec = (uint8_t)(127 - cf); // source=rec decreases as cf increases
    } else if (target == 16 && preset != 16) {
        probRec = cf;                   // target=rec increases as cf increases
    }

    // Max clock rate: 300 BPM × 1/16 = 20 Hz → minimum interval in samples
    const uint32_t minClkInterval = (uint32_t)(NT_globals.sampleRate / 20.0f);

    // --- Per-frame processing ---
    for (int frame = 0; frame < numFrames; ++frame) {

        // Increment cooldown counters (capped to avoid overflow)
        if (p->clkCooldown    < minClkInterval) p->clkCooldown++;
        if (p->shClkCooldown  < minClkInterval) p->shClkCooldown++;
        if (p->recClkCooldown < minClkInterval) p->recClkCooldown++;

        // Output drum bus pulses (buses 37-44 = busFrames index 36-43)
        for (int d = 0; d < SEQ_TRACKS; ++d) {
            float* bus = busFrames + (36 + d) * numFrames + frame;
            if (p->drumPulseCounters[d] > 0) {
                *bus = 5.0f;
                p->drumPulseCounters[d]--;
            } else {
                *bus = 0.0f;
            }
        }

        // Main clock edge
        if (mainClkBus >= 1 && mainClkBus <= 64) {
            float clk = busFrames[(mainClkBus - 1) * numFrames + frame];
            if (clk >= 1.0f && p->lastClock < 0.5f && p->clkCooldown >= minClkInterval) {
                p->clkCooldown = 0;
                // Rising edge: advance drums using main clock
                fireDrumStep(p, false, pulseSamps);
                fireCCSequences(p, midiDest);
                fireTrigSequences(p, alg, midiDest);
            }
            p->lastClock = (clk >= 1.0f) ? 1.0f : (clk < 0.3f ? 0.0f : p->lastClock);
        }

        // Shuffle clock edge
        if (shClkBus >= 1 && shClkBus <= 64) {
            float clk = busFrames[(shClkBus - 1) * numFrames + frame];
            if (clk >= 1.0f && p->lastShuffleClock < 0.5f && p->shClkCooldown >= minClkInterval) {
                p->shClkCooldown = 0;
                fireDrumStep(p, true, pulseSamps);
            }
            p->lastShuffleClock = (clk >= 1.0f) ? 1.0f : (clk < 0.3f ? 0.0f : p->lastShuffleClock);
        }

        // RecClock edge
        if (recClkBus >= 1 && recClkBus <= 64) {
            float clk = busFrames[(recClkBus - 1) * numFrames + frame];
            if (clk >= 1.0f && p->lastRecClock < 0.5f && p->recClkCooldown >= minClkInterval) {
                p->recClkCooldown = 0;
                // Recorder state machine
                switch (p->recorderMode) {
                case REC_LIVE:
                    if (recHigh) {
                        // Start recording
                        p->recEventCount   = 0;
                        p->recCurrentStep  = 0;
                        p->recPlaybackStep = 0;
                        p->recPlayIdx      = 0;
                        uint8_t rl = (uint8_t)alg->v[IDX_REC_LENGTH];
                        p->recTotalSteps   = (uint32_t)rl * 16u;
                        for (int i = 0; i < 4; ++i) { p->noteMask[i] = 0; p->ccMask[i] = 0; }
                        p->recorderMode    = REC_RECORDING;
                    }
                    break;

                case REC_RECORDING:
                    p->recCurrentStep++;
                    if (p->recCurrentStep >= p->recTotalSteps) {
                        // Switch to overdub
                        sortRecBuffer(p);
                        p->recCurrentStep  = 0;
                        p->recPlaybackStep = 0;
                        p->recPlayIdx      = 0;
                        p->recorderMode    = REC_OVERDUB;
                        playbackStep(p, 0, midiDest, pulseSamps, probRec);
                    }
                    break;

                case REC_OVERDUB:
                    p->recCurrentStep = (p->recCurrentStep + 1) % p->recTotalSteps;
                    if (p->recCurrentStep == 0) {
                        sortRecBuffer(p);
                        p->recPlayIdx = 0;
                    }
                    p->recPlaybackStep = p->recCurrentStep;
                    playbackStep(p, p->recPlaybackStep, midiDest, pulseSamps, probRec);
                    // Transition to playback if Record went low
                    // (handled at top of step(), so recorderMode may already be PLAYBACK)
                    break;

                case REC_PLAYBACK:
                    p->recPlaybackStep = (p->recPlaybackStep + 1) % p->recTotalSteps;
                    if (p->recPlaybackStep == 0) p->recPlayIdx = 0;
                    playbackStep(p, p->recPlaybackStep, midiDest, pulseSamps, probRec);
                    // Transition back to overdub if Record went high
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

// ----- Parameter changed -----
static void parameterChanged(_NT_algorithm* alg, int idx) {
    Plugin* p = (Plugin*)alg;

    if (idx == IDX_RANDOMIZE) {
        bool newRand = (alg->v[idx] > 0);
        if (!newRand && p->randomize) genSequences(p); // 0->1 in old was hold, 1->0 was generate
        // Actually: going 0→1 means "hold", going 1→0 means "generate new"
        // So: !newRand && p->randomize → was ON (hold), now OFF → generate
        p->randomize = newRand;
    }
    else if (idx == IDX_RANDOMNESS) {
        p->randomness = (uint8_t)alg->v[idx];
    }
    else if (idx == IDX_FREEZ) {
        bool nf = (alg->v[idx] > 0);
        if (nf && !p->freez) {
            for (int i = 0; i < MAX_SEQS; ++i)
                p->frozenCcVals[i] = getMorphedCC(p, i, p->pos[i]);
        }
        p->freez = nf;
    }
    else if (idx >= IDX_STEPS_BASE && idx < IDX_STEPS_BASE + MAX_SEQS) {
        int seq = idx - IDX_STEPS_BASE;
        p->steps[seq] = (uint8_t)alg->v[idx];
        if (p->randomize) {
            for (int s = 0; s < p->steps[seq]; ++s)
                for (int q = 0; q < 4; ++q)
                    p->ccVals[seq][s][q] = rand() % (p->ranges[seq] + 1);
        }
    }
    else if (idx >= IDX_RANGE_BASE && idx < IDX_RANGE_BASE + MAX_SEQS) {
        int seq = idx - IDX_RANGE_BASE;
        p->ranges[seq] = (uint8_t)alg->v[idx];
        if (p->randomize) {
            for (int s = 0; s < p->steps[seq]; ++s)
                for (int q = 0; q < 4; ++q)
                    p->ccVals[seq][s][q] = rand() % (p->ranges[seq] + 1);
        }
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
        p->seqCurrentPreset = (uint8_t)(alg->v[idx] - 1); // GUI 1-17 → internal 0-16
    }
    else if (idx == IDX_SEQ_TARGET) {
        p->seqTargetPreset = (uint8_t)(alg->v[idx] - 1);
    }
    else if (idx == IDX_SEQ_CROSSFADE) {
        p->seqCrossfade = (uint8_t)alg->v[idx];
    }
    else if (idx == IDX_SEQ_START_STOP) {
        bool ns = (alg->v[idx] > 0);
        if (ns && !p->seqStartStop) {
            for (int t = 0; t < SEQ_TRACKS; ++t) p->seqPos[t] = 0;
        }
        p->seqStartStop = ns;
    }
    else if (idx == IDX_DRM_LENGTHS) {
        uint8_t pr = (uint8_t)(alg->v[idx] - 1);
        if (pr < 8) {
            for (int i = 0; i < 8; ++i)
                p->seqLen[i] = DRUM_LENGTH_PRESETS[pr][i];
        }
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
    // IDX_CLOCK_INPUT, IDX_SHUFFLE_CLK, IDX_REC_CLOCK: just reset edge detectors
    else if (idx == IDX_CLOCK_INPUT) {
        p->lastClock = 0.0f;
    }
    else if (idx == IDX_SHUFFLE_CLK) {
        p->lastShuffleClock = 0.0f;
    }
    else if (idx == IDX_REC_CLOCK) {
        p->lastRecClock = 0.0f;
    }
    // IDX_ZERO, IDX_RECORD, IDX_REC_LENGTH, IDX_LIVE handled in step()
}

// ----- Requirements -----
static void calculateRequirements(_NT_algorithmRequirements& r, const int32_t*) {
    r.numParameters = NUM_PARAMS;
    r.sram = sizeof(Plugin);
    // DRAM for recorder: uint16_t step[2048] + uint8_t b0,b1,b2[2048 each]
    r.dram = (uint32_t)(MAX_REC_EVENTS * 2 + MAX_REC_EVENTS * 3); // 4096 + 6144 = 10240
    r.dtc  = 0;
    r.itc  = 0;
}

// ----- Construct -----
static _NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs,
                                const _NT_algorithmRequirements&,
                                const int32_t*)
{
    Plugin* p = new(ptrs.sram) Plugin();

    // Wire up DRAM recorder buffer
    uint8_t* dram  = (uint8_t*)ptrs.dram;
    p->recStep = (uint16_t*)dram;
    p->recB0   = dram + MAX_REC_EVENTS * 2;
    p->recB1   = dram + MAX_REC_EVENTS * 2 + MAX_REC_EVENTS;
    p->recB2   = dram + MAX_REC_EVENTS * 2 + MAX_REC_EVENTS * 2;

    // CC sequences
    for (int i = 0; i < MAX_SEQS; ++i) {
        p->pos[i]  = 0;
        p->steps[i] = 8;
        p->ranges[i] = 127;
        p->frozenCcVals[i] = 0;
    }
    p->randomize  = false;
    p->randomness = 0;
    p->freez      = false;
    p->morphX     = 0;
    p->morphY     = 0;

    // Clocks
    p->lastClock        = 0.0f;
    p->lastShuffleClock = 0.0f;
    p->lastRecClock     = 0.0f;
    // Init cooldowns to minInterval so the first clock edge is always accepted
    uint32_t initCooldown = (uint32_t)(NT_globals.sampleRate / 20.0f);
    p->clkCooldown    = initCooldown;
    p->shClkCooldown  = initCooldown;
    p->recClkCooldown = initCooldown;

    // Trigger sequences
    for (int i = 0; i < TRIG_SEQS; ++i) {
        p->trigPos[i] = 0;
        p->trigLen[i] = 16;
        for (int s = 0; s < TRIG_STEPS; ++s) {
            p->trigWeights[i][s] = (rand() % 128) + 1;
            p->trigZeroMask[i][s] = 1;
        }
    }
    p->trigThresh = 90;
    p->trigRnd    = false;

    // Step sequencer
    for (int t = 0; t < SEQ_TRACKS; ++t) {
        p->seqPos[t] = 0;
        p->seqLen[t] = 16;
        p->shuffleEnabled[t]    = false;
        p->drumPulseCounters[t] = 0;
    }
    p->seqCurrentPreset = 0;
    p->seqTargetPreset  = 0;
    p->seqCrossfade     = 0;
    p->seqStartStop     = true;

    // Recorder
    clearRecorder(p);
    p->prevRecord = 0;
    p->prevLive   = 0;
    p->recTotalSteps = 64; // 4 bars × 16 steps (default RecLength=4)

    p->parameters     = parameters;
    p->parameterPages = &parameterPages;

    // Generate initial CC sequences
    genSequences(p);

    return (_NT_algorithm*)p;
}

// ----- Entry point -----
extern "C" uintptr_t pluginEntry(_NT_selector s, uint32_t) {
    switch (s) {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo: {
        static const _NT_factory f = {
            .guid                        = NT_MULTICHAR('B','Z','M','S'),
            .name                        = "BizmuthSeq",
            .description                 = "Drum+CC sequencer with recorder",
            .numSpecifications           = 0,
            .specifications              = nullptr,
            .calculateStaticRequirements = nullptr,
            .initialise                  = nullptr,
            .calculateRequirements       = calculateRequirements,
            .construct                   = construct,
            .parameterChanged            = parameterChanged,
            .step                        = pluginStep,
            .draw                        = nullptr,
            .midiRealtime                = nullptr,
            .midiMessage                 = midiMessage,
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
