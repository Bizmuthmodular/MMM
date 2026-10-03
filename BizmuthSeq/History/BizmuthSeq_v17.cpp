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
#define LOOP_BUFFER_SIZE  4   // DIAGNOSTIC: shrunk from 2048 to isolate buffer size vs code logic
#define LOOP_PENDING_POOL   4   // DIAGNOSTIC: shrunk from 64 — see plan

// ----- Parameter indices -----
enum {
    IDX_RANDOMIZE    = 0,
    IDX_RANDOMNESS,           // 1
    IDX_FREEZ,                // 2 - global CC freeze/sample-hold
    IDX_MIDI_OUT,             // 3
    IDX_CLOCK_INPUT,          // 4
    IDX_RESET_IN,             // resets drum/CC/trigger sequences to their first step on a rising edge (0 = disabled)
    IDX_SHUFFLE_IN,           // second (shuffled) clock input, for future use replacing clocking of some voices (0 = disabled)
    IDX_SYNC_IN,              // sync input, for future MIDI looper bar-sync (0 = disabled)
    IDX_DRM_BUS_BASE,         // per-track drum trigger output bus: Kick, Sub, OH, OH2, Snare, CH, Ride, Clap (0 = disabled)
    IDX_TRIGGER_MIDI  = IDX_DRM_BUS_BASE + 8,   // hidden parameter for immediate MIDI triggering
    IDX_PARAM_BASE,           // .. (16 lanes x 2 [Steps, Range])
    CC_NUM_PARAMS     = IDX_PARAM_BASE + (MAX_SEQS*2),

    // Original Trigger generator params
    IDX_TRIG_RND      = CC_NUM_PARAMS,
    IDX_TRIG_THRESH,
    IDX_TRIG_LEN_BASE,                       // ..+TRIG_SEQS

    // Step Sequencer params
    IDX_SEQ_BASE      = IDX_TRIG_LEN_BASE + TRIG_SEQS,
    IDX_SEQ_PRESET,                                     // current preset
    IDX_SEQ_TARGET,                                     // target preset
    IDX_SEQ_CROSSFADE,                                  // crossfade amount
    IDX_SEQ_START_STOP,                                 // start/stop control
    IDX_DRM_LENGTHS,                                     // drum length preset (1-8) - moved from DELTA page
    IDX_MORPH_X,                                         // Morph X coordinate (0-127) - moved from DELTA page
    IDX_MORPH_Y,                                         // Morph Y coordinate (0-127) - moved from DELTA page
    IDX_ZERO,                                            // Zero trigger output (0→1 creates silent holes) - moved from DELTA page

    // Shuffle page: per-track clock source select (0=OFF/main clock, 1=ON/shuffle clock)
    IDX_SHUFFLE_BASE,                                    // ..+8: Kick, Sub, OH, OH2, Snare, CH, Ride, Clap

    // Looper page: MIDI looper controls (RecClock reuses IDX_SYNC_IN — no new routing parameter)
    IDX_LOOP_RECORD   = IDX_SHUFFLE_BASE + 8,            // Record ON/OFF
    IDX_LOOP_REC_LENGTH,                                 // RecLength in bars (1-8)
    IDX_LOOP_LIVE,                                        // Live! trigger (0->1)

    NUM_PARAMS = IDX_LOOP_LIVE + 1
};

// ----- UI enums -----
static const char* enumOnOff[]   = { "OFF", "ON", nullptr };
static const char* enumUsbDin[]  = { "USB", "DIN", "INT+DIN", nullptr };

// ----- Drum Length Presets -----
// 8 presets, each with 6 track lengths (tracks 1-6)
// Order: Kick, Sub, OH, OH2, Snare, CH
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
static const uint8_t SEQUENCE_PRESETS[NUM_PRESETS][8][16] = {
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
        // Track 4: OH2
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
         1,1,1,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         0,0,1,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         1,1,1,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         0,1,1,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         1,1,0,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         0,1,1,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         1,1,1,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         0,1,1,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         1,1,1,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         0,1,1,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         1,1,1,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         0,1,0,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         1,0,1,0},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         1,1,1,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         1,1,1,1},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
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
        // Track 4: OH2
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
         1,1,1,0},
        // Track 7: Ride (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0},
        // Track 8: Clap (empty - pattern-driven placeholder)
        {0,0,0,0,
         0,0,0,0,
         0,0,0,0,
         0,0,0,0}
    }
};

// ----- Parameter storage (EXPERT SLEEPERS PATTERN) -----
static const _NT_parameter parameters[] = {
    { .name = "Randomise!", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },            // IDX_RANDOMIZE
    { .name = "RandomNESS", .min = 0, .max = 127, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_RANDOMNESS
    { .name = "FREEZ!", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },              // IDX_FREEZ

    { .name = "MIDI Out", .min = 0, .max = 2, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumUsbDin },           // IDX_MIDI_OUT - 0=USB, 1=DIN, 2=INT+DIN
    NT_PARAMETER_CV_INPUT( "Clock input", 1, 12 )        // IDX_CLOCK_INPUT - DEFAULT: physical input 12
    NT_PARAMETER_CV_INPUT( "ResetIn", 0, 0 )             // IDX_RESET_IN - resets sequences to step 1 (0 = disabled)
    NT_PARAMETER_CV_INPUT( "ShuffleIn", 0, 0 )           // IDX_SHUFFLE_IN - second/shuffled clock, for future use (0 = disabled)
    NT_PARAMETER_CV_INPUT( "SyncIn", 0, 0 )              // IDX_SYNC_IN - bar-sync input, for future MIDI looper use (0 = disabled)
    NT_PARAMETER_CV_OUTPUT( "KickBus", 0, 0 )            // IDX_DRM_BUS_BASE + 0
    NT_PARAMETER_CV_OUTPUT( "SubBus", 0, 0 )             // IDX_DRM_BUS_BASE + 1
    NT_PARAMETER_CV_OUTPUT( "OHBus", 0, 0 )              // IDX_DRM_BUS_BASE + 2
    NT_PARAMETER_CV_OUTPUT( "OH2Bus", 0, 0 )             // IDX_DRM_BUS_BASE + 3
    NT_PARAMETER_CV_OUTPUT( "SnareBus", 0, 0 )           // IDX_DRM_BUS_BASE + 4
    NT_PARAMETER_CV_OUTPUT( "CHBus", 0, 0 )              // IDX_DRM_BUS_BASE + 5
    NT_PARAMETER_CV_OUTPUT( "RideBus", 0, 0 )            // IDX_DRM_BUS_BASE + 6
    NT_PARAMETER_CV_OUTPUT( "ClapBus", 0, 0 )            // IDX_DRM_BUS_BASE + 7
    { .name = "MIDI Trigger", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },     // IDX_TRIGGER_MIDI

    // CC Parameters - Steps, Range for each of 16 sequences
    { .name = "Steps 1", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 1", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 2", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 2", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 3", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 3", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 4", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 4", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 5", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 5", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 6", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 6", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 7", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 7", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 8", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 8", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 9", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 9", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 10", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 10", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 11", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 11", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 12", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 12", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 13", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 13", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 14", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 14", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 15", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 15", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    { .name = "Steps 16", .min = 1, .max = 16, .def = 8, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },
    { .name = "Range 16", .min = 0, .max = 127, .def = 127, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },

    // Trigger params
    { .name = "Triggers RND", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },         // IDX_TRIG_RND
    { .name = "Threshold", .min = 1, .max = 128, .def = 90, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_TRIG_THRESH

    { .name = "Length 1", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 0
    { .name = "Length 2", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 1
    { .name = "Length 3", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 2
    { .name = "Length 4", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 3
    { .name = "Length 5", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 4
    { .name = "Length 6", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 5
    { .name = "Length 7", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 6
    { .name = "Length 8", .min = 1, .max = 16, .def = 16, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_TRIG_LEN_BASE + 7

    // Step Sequencer params
    { .name = "SeqBase", .min = 0, .max = 0, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },              // IDX_SEQ_BASE (unused marker)
    { .name = "Preset", .min = 1, .max = 16, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },               // IDX_SEQ_PRESET
    { .name = "Target", .min = 1, .max = 16, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },               // IDX_SEQ_TARGET
    { .name = "Crossfade", .min = 0, .max = 127, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_SEQ_CROSSFADE
    { .name = "Start/Stop", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },         // IDX_SEQ_START_STOP
    { .name = "DRMlengths", .min = 1, .max = 8, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },          // IDX_DRM_LENGTHS - moved from DELTA page
    { .name = "MorphX", .min = 0, .max = 127, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_MORPH_X - moved from DELTA page
    { .name = "MorphY", .min = 0, .max = 127, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },            // IDX_MORPH_Y - moved from DELTA page
    { .name = "ZERO", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = enumOnOff },              // IDX_ZERO - moved from DELTA page

    // Shuffle page - per-track clock source select (OFF = main Clock input, ON = ShuffleIn)
    { .name = "Skick", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },   // IDX_SHUFFLE_BASE + 0
    { .name = "Ssub", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },    // IDX_SHUFFLE_BASE + 1
    { .name = "Soh", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },     // IDX_SHUFFLE_BASE + 2
    { .name = "Soh2", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },    // IDX_SHUFFLE_BASE + 3
    { .name = "Ssnare", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },  // IDX_SHUFFLE_BASE + 4
    { .name = "Sch", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },     // IDX_SHUFFLE_BASE + 5
    { .name = "Sride", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },   // IDX_SHUFFLE_BASE + 6
    { .name = "Sclap", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },   // IDX_SHUFFLE_BASE + 7

    // Looper page
    { .name = "Record",    .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },  // IDX_LOOP_RECORD
    { .name = "RecLength", .min = 1, .max = 8, .def = 4, .unit = kNT_unitNone, .scaling = 0, .enumStrings = nullptr },    // IDX_LOOP_REC_LENGTH
    { .name = "Live!",     .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOnOff },  // IDX_LOOP_LIVE
};

// Page indices arrays (Expert Sleepers pattern: static const)
static const uint8_t pageRandIdx[] = {
    IDX_RANDOMIZE,
    IDX_RANDOMNESS,
    IDX_FREEZ
};
static const uint8_t pageMidiOutIdx[] = { IDX_MIDI_OUT };
static const uint8_t pageRouteIdx[] = {
    IDX_CLOCK_INPUT, IDX_RESET_IN, IDX_SHUFFLE_IN, IDX_SYNC_IN,
    IDX_DRM_BUS_BASE+0, IDX_DRM_BUS_BASE+1, IDX_DRM_BUS_BASE+2, IDX_DRM_BUS_BASE+3,
    IDX_DRM_BUS_BASE+4, IDX_DRM_BUS_BASE+5, IDX_DRM_BUS_BASE+6, IDX_DRM_BUS_BASE+7
};
static const uint8_t pageParamIdx[] = {
    IDX_PARAM_BASE+0,  IDX_PARAM_BASE+1,  IDX_PARAM_BASE+2,  IDX_PARAM_BASE+3,
    IDX_PARAM_BASE+4,  IDX_PARAM_BASE+5,  IDX_PARAM_BASE+6,  IDX_PARAM_BASE+7,
    IDX_PARAM_BASE+8,  IDX_PARAM_BASE+9,  IDX_PARAM_BASE+10, IDX_PARAM_BASE+11,
    IDX_PARAM_BASE+12, IDX_PARAM_BASE+13, IDX_PARAM_BASE+14, IDX_PARAM_BASE+15,
    IDX_PARAM_BASE+16, IDX_PARAM_BASE+17, IDX_PARAM_BASE+18, IDX_PARAM_BASE+19,
    IDX_PARAM_BASE+20, IDX_PARAM_BASE+21, IDX_PARAM_BASE+22, IDX_PARAM_BASE+23,
    IDX_PARAM_BASE+24, IDX_PARAM_BASE+25, IDX_PARAM_BASE+26, IDX_PARAM_BASE+27,
    IDX_PARAM_BASE+28, IDX_PARAM_BASE+29, IDX_PARAM_BASE+30, IDX_PARAM_BASE+31
};
static const uint8_t pageTrigIdx[] = {
    IDX_TRIG_RND, IDX_TRIG_THRESH,
    IDX_TRIG_LEN_BASE+0, IDX_TRIG_LEN_BASE+1, IDX_TRIG_LEN_BASE+2, IDX_TRIG_LEN_BASE+3,
    IDX_TRIG_LEN_BASE+4, IDX_TRIG_LEN_BASE+5, IDX_TRIG_LEN_BASE+6, IDX_TRIG_LEN_BASE+7
};
static const uint8_t pageSeqIdx[] = {
    IDX_SEQ_PRESET, IDX_SEQ_TARGET, IDX_SEQ_CROSSFADE, IDX_SEQ_START_STOP,
    IDX_DRM_LENGTHS, IDX_MORPH_X, IDX_MORPH_Y, IDX_ZERO
};
static const uint8_t pageShuffleIdx[] = {
    IDX_SHUFFLE_BASE+0, IDX_SHUFFLE_BASE+1, IDX_SHUFFLE_BASE+2, IDX_SHUFFLE_BASE+3,
    IDX_SHUFFLE_BASE+4, IDX_SHUFFLE_BASE+5, IDX_SHUFFLE_BASE+6, IDX_SHUFFLE_BASE+7
};
static const uint8_t pageLooperIdx[] = {
    IDX_LOOP_RECORD, IDX_LOOP_REC_LENGTH, IDX_LOOP_LIVE
};

// Parameter pages (Expert Sleepers pattern: static const)
static const _NT_parameterPage pages[] = {
    { .name = "RAND",      .numParams = ARRAY_SIZE(pageRandIdx),    .params = pageRandIdx },
    { .name = "MIDI out",  .numParams = ARRAY_SIZE(pageMidiOutIdx), .params = pageMidiOutIdx },
    { .name = "ROUTING",   .numParams = ARRAY_SIZE(pageRouteIdx),   .params = pageRouteIdx },
    { .name = "PARAM",     .numParams = ARRAY_SIZE(pageParamIdx),   .params = pageParamIdx },
    { .name = "TRIGGERS",  .numParams = ARRAY_SIZE(pageTrigIdx),    .params = pageTrigIdx },
    { .name = "SEQUENCER", .numParams = ARRAY_SIZE(pageSeqIdx),     .params = pageSeqIdx },
    { .name = "SHUFFLE",   .numParams = ARRAY_SIZE(pageShuffleIdx), .params = pageShuffleIdx },
    { .name = "LOOPER",    .numParams = ARRAY_SIZE(pageLooperIdx),  .params = pageLooperIdx }
};

static const _NT_parameterPages parameterPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages = pages
};

// ----- MIDI Looper types -----
enum { kLoopLive = 0, kLoopRecording = 1, kLoopOverdub = 2, kLoopPlayback = 3 };

// One recorded MIDI event. uint32_t first, then four uint8_t: sizeof == 8, no padding.
struct LoopEvent {
    uint32_t offset;   // samples elapsed since this step's RecClock rising edge, at capture time
    uint8_t  step;     // loop step index this event belongs to (0 .. 16*RecLength-1, max 127)
    uint8_t  status;   // raw MIDI status byte, channel already OR'd in (0x80-0xEF)
    uint8_t  data1;
    uint8_t  data2;
};

// One armed, not-yet-fired scheduled playback event. sizeof == 8, no padding.
struct LoopPendingEvent {
    uint32_t countdown;  // samples remaining until fire (0 = fire this frame)
    uint8_t  status;
    uint8_t  data1;
    uint8_t  data2;
    bool     active;     // true = slot in use
};

// ----- Plugin state -----
struct Plugin : _NT_algorithm {
    // CC sequences - 4 sequences per track for 2D morphing
    // [track][step][sequence] where sequence 0=LL, 1=LR, 2=UL, 3=UR
    uint8_t ccVals[MAX_SEQS][MAX_STEPS][4];
    uint8_t pos[MAX_SEQS];
    uint8_t steps[MAX_SEQS], ranges[MAX_SEQS];
    bool    randomize;
    uint8_t randomness;
    bool    freez;                          // NEW: Global CC freeze state
    uint8_t frozenCcVals[MAX_SEQS];        // NEW: Frozen CC values for sample-hold
    float   lastClock;
    float   lastReset;                     // Previous ResetIn sample, for edge detection
    float   lastShuffleClock;              // Previous ShuffleIn sample, for edge detection
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
    uint8_t seqPosMain[SEQ_TRACKS];             // Current position per track, main-clock stream (always advances on Clock input)
    uint8_t seqPosShuffle[SEQ_TRACKS];          // Current position per track, shuffle-clock stream (always advances on ShuffleIn)
    uint8_t seqLen[SEQ_TRACKS];                // Length per track (1-16)
    uint8_t seqCurrentPreset;                  // Current preset index
    uint8_t seqTargetPreset;                   // Target preset for crossfade
    uint8_t seqCrossfade;                      // Crossfade amount (0-127)
    bool    seqStartStop;                      // Start/stop control

    // 1/16 clock output (CC#1, Channel 7)
    uint16_t clockPulseCounter;                // Sample counter for 15ms clock pulse

    // Drum trigger bus outputs: 5ms 10V pulse per track
    uint16_t drumPulseCounter[8];               // Sample counter per track for 5ms bus pulse

    // MIDI looper state deliberately does NOT live here — see the plain static globals below.
};

// MIDI looper state, as plain independent static globals — no struct-of-pointers indirection,
// no manual pointer-arithmetic carving of one big block into typed sub-regions (that pattern
// is unprecedented elsewhere in this project; every other static/DRAM buffer here is a single
// flat cast). sizeof(Plugin) must stay at exactly v15's 1584 bytes (confirmed hard requirement
// this session — even 4 bytes more froze the module on load), so none of this can live in the
// Plugin/SRAM struct either. BizmuthSeq is used as a single instance per preset, so shared
// static state here is an accepted limitation, not a bug.
static LoopEvent        g_loopBuffer[LOOP_BUFFER_SIZE];
static LoopPendingEvent g_loopPending[LOOP_PENDING_POOL];
static uint8_t          g_noteMask[16][16];
static uint8_t          g_ccMask[16][16];
static uint16_t         g_loopEventCount = 0;
static uint8_t          g_looperMode = kLoopLive;
static uint8_t          g_loopStep = 0;
static uint8_t          g_recLengthBars = 4;
static bool             g_loopRecordOn = false;
static bool             g_loopRecordArmed = false;
static bool             g_loopLivePrev = false;
static float            g_lastRecClock = 0.0f;
static uint32_t         g_samplesSinceRecClockEdge = 0;

// ----- Generate CC sequences -----
// Generates 4 random sequences per track for 2D morphing
static void genSequences(Plugin* p) {
    for(int i=0; i<MAX_SEQS; ++i) {
        for(int s=0; s<p->steps[i]; ++s) {
            // Generate 4 sequences: LL (0), LR (1), UL (2), UR (3)
            for(int seq=0; seq<4; ++seq) {
                p->ccVals[i][s][seq] = rand() % (p->ranges[i] + 1);
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
        p->seqPosMain[t] = 0;
        p->seqPosShuffle[t] = 0;
    }
    p->seqCurrentPreset = 0;
    p->seqTargetPreset = 0;
    p->seqCrossfade = 0;
    p->seqStartStop = true;

    // Load initial preset
    loadPreset(p, 0);
}

// ----- Advance one drum track by one step in the given position stream.
//       Always advances; only fires its MIDI note and bus pulse if 'fire' is true. -----
static void stepDrumTrack(Plugin* p, int t, uint32_t dest, uint8_t* streamPos, bool fire) {
    uint8_t pos = streamPos[t];

    if (fire) {
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

        if (stepToPlay) {
            NT_sendMidi3ByteMessage(dest, 0x90 | 3, (uint8_t)(t + 8), 127);
            p->drumPulseCounter[t] = (uint16_t)(NT_globals.sampleRate * 0.005f);
        }
    }

    streamPos[t] = (streamPos[t] + 1) % p->seqLen[t];
}

// ----- Scan the recorded loop buffer and rebuild the Playback-mode note/CC identity masks -----
static void buildLoopMask() {
    memset(g_noteMask, 0, 16 * 16);
    memset(g_ccMask,   0, 16 * 16);
    for (uint16_t i = 0; i < g_loopEventCount; ++i) {
        const LoopEvent& ev = g_loopBuffer[i];
        const uint8_t hiNibble = ev.status & 0xF0;
        const uint8_t channel  = ev.status & 0x0F;
        if (hiNibble == 0x80 || hiNibble == 0x90)
            g_noteMask[channel][ev.data1 >> 3] |= (uint8_t)(1 << (ev.data1 & 7));
        else if (hiNibble == 0xB0)
            g_ccMask[channel][ev.data1 >> 3] |= (uint8_t)(1 << (ev.data1 & 7));
    }
}

// ----- Send a standard MIDI "All Notes Off" Channel Mode Message (CC 123) on every channel -----
static void sendAllNotesOff(uint32_t dest) {
    for (int ch = 0; ch < 16; ++ch) {
        NT_sendMidi3ByteMessage(dest, (uint8_t)(0xB0 | ch), 123, 0);
    }
}

// ----- Audio step: 1 pattern step per rising clock edge -----
extern "C" void pluginStep(_NT_algorithm* alg, float* busFrames, int numFramesBy4) {
    Plugin* p = (Plugin*)alg;

    const int numFrames = numFramesBy4 * 4;
    const int busParam  = alg->v[IDX_CLOCK_INPUT];

    // Pre-compute clock bus pointer once before loop (Chords pattern)
    const float* clockIn = (busParam >= 1 && busParam <= kNT_lastBus)
                           ? busFrames + (busParam - 1) * numFrames : nullptr;

    // Pre-compute reset bus pointer
    const int resetParam = alg->v[IDX_RESET_IN];
    const float* resetIn = (resetParam >= 1 && resetParam <= kNT_lastBus)
                           ? busFrames + (resetParam - 1) * numFrames : nullptr;

    // Pre-compute shuffle clock bus pointer
    const int shuffleParam = alg->v[IDX_SHUFFLE_IN];
    const float* shuffleIn = (shuffleParam >= 1 && shuffleParam <= kNT_lastBus)
                           ? busFrames + (shuffleParam - 1) * numFrames : nullptr;

    // Pre-compute RecClock (looper bar-sync) bus pointer — reuses existing IDX_SYNC_IN parameter
    const int recClockParam = alg->v[IDX_SYNC_IN];
    const float* recClockIn = (recClockParam >= 1 && recClockParam <= kNT_lastBus)
                           ? busFrames + (recClockParam - 1) * numFrames : nullptr;

    // Pre-compute each drum track's own dedicated output bus pointer
    float* drumOut[8];
    for (int t = 0; t < 8; ++t) {
        int busNum = alg->v[IDX_DRM_BUS_BASE + t];
        drumOut[t] = (busNum >= 1 && busNum <= kNT_lastBus)
                     ? busFrames + (busNum - 1) * numFrames : nullptr;
    }

    uint32_t dest;
    switch (alg->v[IDX_MIDI_OUT]) {
        case 1:  dest = (uint32_t)kNT_destinationBreakout; break;
        case 2:  dest = (uint32_t)kNT_destinationBreakout | (uint32_t)kNT_destinationInternal; break;
        default: dest = (uint32_t)kNT_destinationUSB; break;
    }

    for (int frame = 0; frame < numFrames; ++frame) {

        if (clockIn) {
            const float clk = clockIn[frame];

            // Rising edge: was below 1V, now at or above 1V — no hysteresis dead zone
            if (p->lastClock < 1.0f && clk >= 1.0f) {

                // --- Drum tracks 1-8: main-clock stream always advances; fires only for
                //     tracks NOT assigned to the shuffle clock (Skick..Sclap == OFF) ---
                if (p->seqStartStop) {
                    for (int t = 0; t < 8; ++t) {
                        bool fire = !alg->v[IDX_SHUFFLE_BASE + t];
                        stepDrumTrack(p, t, dest, p->seqPosMain, fire);
                    }
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
                        p->pos[i] = (p->pos[i] + 1) % p->steps[i];
                    }

                    NT_sendMidi3ByteMessage(dest, 0xB0 | 2, 17 + i, val);
                }

                // 1/16 clock output pulse
                NT_sendMidi3ByteMessage(dest, 0xB0 | 6, 1, 127);
                p->clockPulseCounter = (uint16_t)(NT_globals.sampleRate * 0.015f);

                p->globalClockCount++;
            }

            // Always store raw clock value — no dead zone (Chords pattern)
            p->lastClock = clk;
        }

        if (resetIn) {
            const float rst = resetIn[frame];

            // Rising edge: reset drum, CC, and trigger sequences to their first step
            if (p->lastReset < 1.0f && rst >= 1.0f) {
                for (int t = 0; t < 8; ++t) {
                    p->seqPosMain[t] = 0;
                    p->seqPosShuffle[t] = 0;
                }
                for (int i = 0; i < MAX_SEQS; ++i) {
                    p->pos[i] = 0;
                }
                for (int i = 0; i < TRIG_SEQS; ++i) {
                    p->trigPos[i] = 0;
                }
            }

            p->lastReset = rst;
        }

        if (shuffleIn) {
            const float shf = shuffleIn[frame];

            // Rising edge: shuffle-clock stream always advances; fires only for
            // tracks assigned to the shuffle clock (Skick..Sclap == ON)
            if (p->lastShuffleClock < 1.0f && shf >= 1.0f) {
                if (p->seqStartStop) {
                    for (int t = 0; t < 8; ++t) {
                        bool fire = alg->v[IDX_SHUFFLE_BASE + t] != 0;
                        stepDrumTrack(p, t, dest, p->seqPosShuffle, fire);
                    }
                }
            }

            p->lastShuffleClock = shf;
        }

        if (recClockIn) {
            const float rc = recClockIn[frame];

            if (g_lastRecClock < 1.0f && rc >= 1.0f) {
                // RecClock rising edge

                if (g_loopRecordArmed) {
                    // First edge after Record went high: start a fresh recording pass
                    g_loopEventCount  = 0;
                    g_loopStep        = 0;
                    g_looperMode      = kLoopRecording;
                    g_loopRecordArmed = false;
                } else {
                    // Advance loop step, wrap at 16 * RecLength
                    const uint8_t loopLenSteps = (uint8_t)(16 * g_recLengthBars);
                    g_loopStep++;
                    if (g_loopStep >= loopLenSteps) {
                        g_loopStep = 0;
                        if (g_looperMode == kLoopRecording)
                            g_looperMode = kLoopOverdub;   // auto transition at RecLength boundary
                    }
                }

                // Arm pending playback for events recorded at this step (Overdub plays back +
                // layers; Playback plays back only). Not done in kLoopRecording — nothing to play yet.
                if (g_looperMode == kLoopOverdub || g_looperMode == kLoopPlayback) {
                    for (uint16_t e = 0; e < g_loopEventCount; ++e) {
                        if (g_loopBuffer[e].step == g_loopStep) {
                            for (int s = 0; s < LOOP_PENDING_POOL; ++s) {
                                if (!g_loopPending[s].active) {
                                    g_loopPending[s].active    = true;
                                    g_loopPending[s].countdown = g_loopBuffer[e].offset;
                                    g_loopPending[s].status    = g_loopBuffer[e].status;
                                    g_loopPending[s].data1     = g_loopBuffer[e].data1;
                                    g_loopPending[s].data2     = g_loopBuffer[e].data2;
                                    break;
                                }
                            }
                        }
                    }
                }

                g_samplesSinceRecClockEdge = 0;
            }

            g_lastRecClock = rc;
        }

        if (p->clockPulseCounter > 0) {
            if (--p->clockPulseCounter == 0)
                NT_sendMidi3ByteMessage(dest, 0xB0 | 6, 1, 0);
        }

        // --- Drum trigger bus outputs: 5ms 10V pulse per track ---
        for (int t = 0; t < 8; ++t) {
            if (drumOut[t] && p->drumPulseCounter[t] > 0) {
                drumOut[t][frame] += 10.0f;
                --p->drumPulseCounter[t];
            }
        }

        // --- Looper: fire any pending scheduled playback events whose countdown has elapsed ---
        for (int s = 0; s < LOOP_PENDING_POOL; ++s) {
            if (g_loopPending[s].active) {
                if (g_loopPending[s].countdown == 0) {
                    NT_sendMidi3ByteMessage(dest, g_loopPending[s].status,
                                             g_loopPending[s].data1, g_loopPending[s].data2);
                    g_loopPending[s].active = false;
                } else {
                    --g_loopPending[s].countdown;
                }
            }
        }

        // --- Looper: advance samples-since-RecClock-edge counter (read by midiMessage()) ---
        g_samplesSinceRecClockEdge++;

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
    else if (idx == IDX_CLOCK_INPUT) {
        p->lastClock = p->lastClockTrig = 0.0f;
    }
    else if (idx == IDX_RESET_IN) {
        p->lastReset = 0.0f;
    }
    else if (idx == IDX_SHUFFLE_IN) {
        p->lastShuffleClock = 0.0f;
    }
    else if (idx >= IDX_PARAM_BASE && idx < CC_NUM_PARAMS) {
        int seq = (idx - IDX_PARAM_BASE) / 2, f = (idx - IDX_PARAM_BASE) % 2;
        int16_t v = alg->v[idx];
        if (f == 0) p->steps[seq]  = v;
        if (f == 1) p->ranges[seq] = v;
        if (p->randomize) {
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
                p->seqPosMain[t] = 0;
                p->seqPosShuffle[t] = 0;
            }
            p->globalClockCount = 0;  // Reset global clock for fresh start
        }
        p->seqStartStop = newState;
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
    else if (idx == IDX_SYNC_IN) {
        g_lastRecClock = 0.0f;
    }
    else if (idx == IDX_LOOP_RECORD) {
        bool newRecord = alg->v[idx];
        if (newRecord && !g_loopRecordOn) {
            if (g_looperMode == kLoopPlayback) {
                // Loop already exists and is playing: resume immediately into Overdub,
                // keep the existing buffer/loop length as-is, no wait for a RecClock edge.
                g_looperMode = kLoopOverdub;
            } else {
                // Live mode (empty buffer): wait for the next RecClock edge to start a fresh take
                g_loopRecordArmed = true;
            }
        } else if (!newRecord && g_loopRecordOn) {
            g_loopRecordArmed = false;
            if (g_looperMode == kLoopRecording || g_looperMode == kLoopOverdub) {
                g_looperMode = kLoopPlayback;
                buildLoopMask();

                uint32_t dest;
                switch (alg->v[IDX_MIDI_OUT]) {
                    case 1:  dest = (uint32_t)kNT_destinationBreakout; break;
                    case 2:  dest = (uint32_t)kNT_destinationBreakout | (uint32_t)kNT_destinationInternal; break;
                    default: dest = (uint32_t)kNT_destinationUSB; break;
                }
                sendAllNotesOff(dest);
            }
        }
        g_loopRecordOn = newRecord;
    }
    else if (idx == IDX_LOOP_REC_LENGTH) {
        g_recLengthBars = (uint8_t)alg->v[idx];
    }
    else if (idx == IDX_LOOP_LIVE) {
        bool newLive = alg->v[idx];
        if (newLive && !g_loopLivePrev) {
            // Rising edge: clear the recording, return to Live/pass-through.
            // Set our own state first, then force Record=0 — NT_setParameterFromAudio may
            // re-enter parameterChanged(IDX_LOOP_RECORD) synchronously; by then looperMode
            // is already kLoopLive so that branch's mode-changing guard is skipped.
            g_loopEventCount  = 0;
            g_looperMode      = kLoopLive;
            g_loopRecordArmed = false;

            uint32_t dest;
            switch (alg->v[IDX_MIDI_OUT]) {
                case 1:  dest = (uint32_t)kNT_destinationBreakout; break;
                case 2:  dest = (uint32_t)kNT_destinationBreakout | (uint32_t)kNT_destinationInternal; break;
                default: dest = (uint32_t)kNT_destinationUSB; break;
            }
            sendAllNotesOff(dest);

            NT_setParameterFromAudio(NT_algorithmIndex(alg), IDX_LOOP_RECORD + NT_parameterOffset(), 0);
        }
        g_loopLivePrev = newLive;
    }
}

// ----- MIDI input: pass-through / record / masked playback for the looper -----
extern "C" void midiMessage(_NT_algorithm* self, uint8_t byte0, uint8_t byte1, uint8_t byte2) {
    uint32_t dest;
    switch (self->v[IDX_MIDI_OUT]) {
        case 1:  dest = (uint32_t)kNT_destinationBreakout; break;
        case 2:  dest = (uint32_t)kNT_destinationBreakout | (uint32_t)kNT_destinationInternal; break;
        default: dest = (uint32_t)kNT_destinationUSB; break;
    }

    const uint8_t status   = byte0;
    const uint8_t hiNibble = byte0 & 0xF0;
    const uint8_t channel  = byte0 & 0x0F;
    const bool isNote = (hiNibble == 0x80 || hiNibble == 0x90);
    const bool isCC   = (hiNibble == 0xB0);

    switch (g_looperMode) {

        case kLoopRecording:
        case kLoopOverdub:
            // Always pass through live, unconditionally, in both modes.
            NT_sendMidi3ByteMessage(dest, status, byte1, byte2);
            // Capture, if there's room (buffer-full grace: keep passing through, just stop recording)
            if (g_loopEventCount < LOOP_BUFFER_SIZE) {
                LoopEvent& ev = g_loopBuffer[g_loopEventCount];
                ev.offset = g_samplesSinceRecClockEdge;   // read only — pluginStep() owns writes
                ev.step   = g_loopStep;
                ev.status = status;
                ev.data1  = byte1;
                ev.data2  = byte2;
                g_loopEventCount++;
            }
            break;

        case kLoopPlayback: {
            bool masked = false;
            if (isNote)    masked = (g_noteMask[channel][byte1 >> 3] & (1 << (byte1 & 7))) != 0;
            else if (isCC) masked = (g_ccMask[channel][byte1 >> 3]   & (1 << (byte1 & 7))) != 0;
            // Any other channel-message type (Program Change, Aftertouch, Pitch Bend) has no
            // identity in the masking rule as specified, so it is never masked.
            if (!masked) NT_sendMidi3ByteMessage(dest, status, byte1, byte2);
            break;
        }

        case kLoopLive:
        default:
            NT_sendMidi3ByteMessage(dest, status, byte1, byte2);
            break;
    }
}

// ----- Requirements -----
extern "C" void calculateRequirements(_NT_algorithmRequirements& r, const int32_t*) {
    r.numParameters = ARRAY_SIZE(parameters);  // Expert Sleepers pattern
    r.sram          = sizeof(Plugin);
    r.dram          = 0;  // looper state is plain static globals now, no per-instance DRAM needed
    r.dtc = r.itc   = 0;
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
    p->lastReset     = 0.0f;
    p->lastShuffleClock = 0.0f;
    p->globalClockCount = 0;  // NEW: Initialize global clock counter

    // MIDI looper state: plain static globals (see declarations above), reset on each construct.
    g_loopEventCount           = 0;
    g_looperMode                = kLoopLive;
    g_loopStep                  = 0;
    g_loopRecordOn              = false;
    g_loopRecordArmed           = false;
    g_loopLivePrev               = false;
    g_lastRecClock               = 0.0f;
    g_samplesSinceRecClockEdge  = 0;
    memset(g_loopPending, 0, LOOP_PENDING_POOL * sizeof(LoopPendingEvent));
    memset(g_noteMask,    0, 16 * 16);
    memset(g_ccMask,      0, 16 * 16);

    // Initialize positions and counters (not parameters)
    for (int i = 0; i < MAX_SEQS; ++i) {
        p->pos[i] = 0;
        p->frozenCcVals[i] = 0;  // Initialize frozen CC values
    }
    for (int i = 0; i < TRIG_SEQS; ++i) {
        p->trigPos[i] = 0;
    }

    // Initialize 1/16 clock output
    p->clockPulseCounter = 0;       // No active clock pulse

    // Initialize drum trigger bus outputs
    for (int i = 0; i < 8; ++i) {
        p->drumPulseCounter[i] = 0;  // No active pulse
    }

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
                "8-track drum + 16-lane CC sequencer",
                0, nullptr,
                nullptr, nullptr,
                calculateRequirements,
                construct,
                parameterChanged,  // Keep parameterChanged - essential for plugin functionality
                pluginStep,
                nullptr, nullptr, midiMessage,
                kNT_tagUtility,
                nullptr, nullptr, nullptr
            };
            return (uintptr_t)&f;
        }
    default: return 0 ;
    }
}