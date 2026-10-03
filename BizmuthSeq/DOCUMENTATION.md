# BizmuthSeq Plugin Documentation

**Version:** 1.0
**Platform:** Expert Sleepers disting NT
**Last Updated:** 2025-12-28

---

## Overview

BizmuthSeq is a looped sequential random midi CC generator plus a 6-track drum sequencer with sub-step offset control and an integrated burst generator for the Expert Sleepers disting NT Eurorack module. It receives an 8x fast clock input and provides precise timing control for drum patterns with microsecond-level offsets.

---

## Key Features

0. Generates 16 Midi CC sequences based on multiple parameters. 

### 1. 8x Clock-Based Sequencing
- **Input:** Receives 8x speed clock (8 ticks per musical step)
- **Tracks:** 6 independent drum tracks (Kick, Sub, OH, Ride, Snare, CH)
- **MIDI Output:** Notes 8-13 on Channel 4
- **Track Assignments:**
  - Track 1 (Kick): MIDI Note 8
  - Track 2 (Sub): MIDI Note 9 - **CONTROLLED BY BURST GENERATOR**
  - Track 3 (OH): MIDI Note 10
  - Track 4 (Ride): MIDI Note 11
  - Track 5 (Snare): MIDI Note 12
  - Track 6 (CH): MIDI Note 13

### 2. Sub-Step Offsets
- **Range:** -8 to +8 clock ticks per track
- **Resolution:** 1/8th of a musical step
- **Application:** Each track can be shifted in time relative to the base clock
- **Control:** Individual offset parameters per track (KICKOFF, SUBOFF, etc.)
- **Enable/Disable:** Global OffsetsON switch
- **Randomization:**
  - RNDOffsets: Trigger button (0→1 transition randomizes)
  - RNDoffAMT: Range limiter (0-8) for random values

### 3. Burst Generator (Track 2)
The burst generator replaces Track 2's pattern-based output with a snare-triggered burst sequence.

**Trigger Source:**
- Snare (Track 5, MIDI Note 12) triggers burst sequence
- Trigger occurs AFTER snare's sub-step offset is applied

**Parameters:**
- **BurstRate:** 1.0-20.0 Hz (0.1 Hz resolution)
  - Internal range: 10-200 (displayed value / 10)
  - Controls clock rate for burst sequence

- **BCount:** 0-8 bursts
  - Number of 4ms pulses to output per sequence
  - 0 = burst generator disabled

- **BSpRand:** ON/OFF
  - Enables/disables timing randomization

- **BrndAMT:** 0-100%
  - Randomization amount (percentage of phase deviation)
  - Generates 8-pattern random offset array
  - Pattern repeats cyclically (bursts 0-7, 8-15, etc.)

**Behavior:**
1. Snare fires → Reset burst phase accumulator → Activate burst sequence
2. Phase accumulator runs at BurstRate frequency
3. Each phase wrap fires next burst (4ms MIDI Note ON pulse)
4. Random offsets applied per burst based on pattern index (current count % 8)
5. After BCount bursts, sequence stops until next snare trigger
6. If new snare fires before sequence completes, old sequence is canceled

**Output:**
- MIDI Note 9, Channel 4 (same as original Track 2)
- Fixed 4ms pulse width (192 samples at 48kHz)
- Broadcast to all MIDI destinations (~0)

---

## Technical Implementation

### Clock Processing
- **Global Clock Counter:** Tracks 8x clock ticks since start
- **Cycle Length:** Steps × 8 (e.g., 16 steps = 128 clock ticks)
- **Trigger Timing:** `baseClockForStep = step * 8 + trackOffset`
- **Wrap Handling:** Offsets wrapped to valid cycle range using modulo

### Phase Accumulator (Burst Generator)
- **32-bit phase accumulator:** 0 to 4,294,967,295 (wraps at overflow)
- **Phase increment calculation:**
  ```cpp
  frequencyHz = burstRate / 10.0f  // Convert from scaled value
  phaseIncrement = (2^32 / sampleRate) * frequencyHz
  ```
- **Randomization:**
  ```cpp
  maxOffset = (phaseIncrement * burstRndAmt) / 100
  randomOffset = (rand() % (maxOffset * 2 + 1)) - maxOffset
  phaseIncrementWithOffset = phaseIncrement + randomOffsets[burstCount % 8]
  ```

### Sample-Accurate Processing
- **Main loop:** Processes each audio sample individually (48kHz)
- **Clock detection:** Rising edge when `clkSample > 0.7V` after being `< 0.3V`
- **Burst phase:** Increments every sample when active
- **4ms pulse counter:** Counts down from 192 samples (48000 * 0.004)

### Preset System
- **8 presets** for tracks 1-6
- **Crossfade:** Probabilistic blend between current and target preset
- **Tracks 7-8:** Fixed patterns (not preset-controlled)

---

## Parameter Index Reference

```cpp
IDX_CLOCK_INPUT     = 0     // Clock input bus (1-28)
IDX_MIDI_OUT        = 1     // MIDI output routing (USB/Breakout)
IDX_RANDOMIZE       = 2     // Randomize CC sequences
IDX_RANDOMNESS      = 3     // Randomness amount
IDX_FREEZ           = 4     // Freeze CC sequences

// CC Control (5-20)
IDX_INCLUDE_BASE    = 5     // Include flags for 16 CCs
IDX_PARAM_BASE      = 21    // CC numbers (16 params)
IDX_RANGE_BASE      = 37    // CC ranges (16 params)
IDX_STEPS_BASE      = 53    // Step counts (16 params)
IDX_DIVS_BASE       = 69    // Clock divisions (16 params)

// Trigger Control (85-92)
IDX_TRIG_RND        = 85    // Randomize triggers
IDX_TRIG_THRESH     = 86    // Trigger threshold
IDX_TRIG_LEN_BASE   = 87    // Trigger lengths (4 params)

// Step Sequencer (91-106)
IDX_SEQ_START_STOP  = 91    // Start/stop sequencer
IDX_SEQ_PRESET      = 92    // Current preset (0-7)
IDX_SEQ_CROSSFADE   = 93    // Crossfade amount (0-127)
IDX_SEQ_TRACK_LENS  = 94    // Track lengths (6 params, tracks 1-6)

// Sub-Step Offsets (107-115)
IDX_OFFSET_BASE     = 107   // Track offsets (6 params: KICKOFF, SUBOFF, etc.)
IDX_OFFSETS_ON      = 113   // Enable/disable offsets
IDX_RND_OFFSETS     = 114   // Randomize offsets (0→1 trigger)
IDX_RND_OFF_AMT     = 115   // Randomization range (0-8)

// Burst Generator (116-119)
IDX_BURST_RATE      = 116   // Burst rate (10-200 = 1.0-20.0 Hz)
IDX_BURST_COUNT     = 117   // Burst count (0-8)
IDX_BURST_RND       = 118   // Burst randomization (ON/OFF)
IDX_BURST_RND_AMT   = 119   // Burst random amount (0-100%)

NUM_PARAMS          = 120   // Total parameters (0-119)
```

---

## Parameter Pages

1. **RAND** - CC randomization controls
2. **MIDI out** - MIDI routing
3. **ROUTING** - Clock input selection
4. **PARAM** - CC numbers (16 CCs)
5. **TRIG** - Trigger control
6. **RANGE** - CC value ranges
7. **STEPS** - Sequence step counts
8. **DIVS** - Clock divisions
9. **SEQ** - Step sequencer controls
10. **OFFSET** - Sub-step offsets and burst generator

---

## Memory Layout

### Plugin Struct Size
- **CC Sequences:** 16 × 128 × 1 byte = 2048 bytes
- **Frozen CC Values:** 16 × 1 byte = 16 bytes
- **Trigger Weights:** 4 × 128 × 1 byte = 512 bytes
- **Step Sequencer Data:** 6 × 16 × 1 byte = 96 bytes
- **Sub-step Offsets:** 6 × 1 byte = 6 bytes
- **Burst Generator:** ~60 bytes
  - Phase accumulator: 4 bytes
  - Random offsets: 8 × 4 = 32 bytes
  - Control vars: ~24 bytes
- **Total:** ~2.8 KB SRAM

---

## MIDI Implementation

### Note Output
- **Channel:** 4 (MIDI channel index 3)
- **Note ON:** 0x93 (144 + 3)
- **Note OFF:** 0x83 (128 + 3)
- **Velocity:** 127 (fixed)
- **Destination:** Broadcast (~0) or user-selected (USB/Breakout)

### CC Output
- **Channel:** 3 (MIDI channel index 2)
- **Controllers:** 17-32 (configurable via PARAM page)
- **Values:** 0-127 (range limited per CC)

---

## Compilation

**Toolchain:** ARM GCC (arm-none-eabi-g++)
**Target:** ARM Cortex-M7
**FPU:** FPv5-D16 (hard float ABI)
**Optimization:** -Os (optimize for size)

**Build command:**
```bash
make bizmuthseq
```

**Output:** `BizmuthSeq.o`

---

## Known Limitations

### Burst Generator
- **Maximum stable frequency:** ~17-18 Hz due to sample-rate timing quantization
  - At higher rates, periodic skips/jumps may occur
  - Recommended range: 1.0-15.0 Hz for stable operation
  - Extended range to 20 Hz available but may exhibit timing artifacts

### Clock Input
- **Voltage thresholds:** High = 0.7V, Low = 0.3V
- **Edge detection:** Rising edge only
- **No clock division validation:** User must provide correct 8x clock

### MIDI Timing
- **Sample-accurate trigger timing** (48kHz resolution)
- **4ms pulse width** fixed for burst generator
- **No velocity sensitivity**

---

## Randomization Behavior

### CC Sequences (RANDOMIZE)
- Generates new random values for all included CCs
- Values: 0 to range (configurable per CC)
- Applies immediately on parameter change

### Trigger Weights (TRIG_RND)
- Generates random weights 1-128 for trigger patterns
- Each trigger sequence has 128 steps
- Threshold comparison determines output

### Sub-Step Offsets (RND_OFFSETS)
- **Edge-triggered:** Only on 0→1 transition
- Generates random offset -RNDoffAMT to +RNDoffAMT
- Applies to all 6 tracks simultaneously
- Clamped to ±8 range during playback

### Burst Timing (BSpRand)
- Generates 8-value pattern array
- Each value: ±(phaseIncrement × BrndAMT / 100)
- Pattern repeats cyclically
- Regenerated when BSpRand or BrndAMT changes

---

## API Compliance

**Compatible with:** Expert Sleepers disting NT Plugin API v1.x
**Uses:**
- `NT_globals.sampleRate` for sample rate (typically 48kHz)
- `NT_sendMidi3ByteMessage()` for MIDI output
- `kNT_unitHz`, `kNT_unitPercent`, `kNT_unitNone` for parameter units
- `kNT_scaling10` for 0.1 Hz resolution
- `kNT_destinationUSB`, `kNT_destinationBreakout` for MIDI routing

**Avoids:**
- 64-bit division (not supported - causes invalid plugin)
- Physical CV output on bus frames (Track 2 uses MIDI only)
- Integer division for timing (uses floating-point phase accumulator)

---

## Version History

**v1.0 (2025-12-28)**
- Initial release
- 8x clock-based sequencing with sub-step offsets
- Burst generator on Track 2
- 0.1 Hz resolution for burst rate (1.0-20.0 Hz range)
- Fixed 4ms pulse width
- Snare-triggered burst sequences with randomization

---

## Future Considerations

**Potential Improvements:**
- Per-burst velocity control
- Variable pulse width parameter
- Multiple trigger sources (not just snare)
- Burst output to different MIDI note
- Clock division for burst rate (musical timing)
- Burst euclidean pattern mode

**Performance Optimizations:**
- Move burst processing outside per-sample loop if stable timing at >18 Hz needed
- Pre-calculate phase thresholds for fixed duty cycles
- Use lookup tables for randomization

---

## Contact & Support

This plugin was developed for the Expert Sleepers disting NT.
For API documentation: https://www.expert-sleepers.co.uk/distingNT.html

---

## License

*License information to be determined by plugin author*
