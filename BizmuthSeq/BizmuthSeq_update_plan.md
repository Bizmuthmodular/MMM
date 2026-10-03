# BizmuthSeq Update Plan

most important rule: keep the plugin functional by preserving programming methods 
and working principals from v3 everywhere where its possible. 

---



## REMOVALS

- **Offset page**: all 13 parameters gone (KickOFF–CHOFF, OffsetsON, RNDOffsets, RNDoffAMT, BurstRate, BCount, BSpRand, BrndAMT)
- **Burst generator**: removed entirely
- **Delta page**: removed entirely (Delta1–16, DeltaAMT, DeltaScope, DeltaShape) — DRMlengths, MorphX, MorphY, ZERO move to SEQUENCER page
- **Individual drum track length params (Seq Len 1–6)** on SEQUENCER page: removed — DRMlengths presets now control all lengths additional tracks are 16 steps long at all length preset settings for now. 
- **Include 1–16** on RAND page: removed (16 params only; Randomise!, RandomNESS, FREEZ! remain)
- **PARAM page**: remove Div 1–16 and Dir 1–16, keep Steps 1–16 and Range 1–16 only


---



---

## DRUM OUTPUT — BUS TRIGGERS

All drum tracks output 5ms high pulses on dedicated buses:

| Track | Voice         | Bus |
|-------|--------------|-----|
| 1     | Kick         | 37  |
| 2     | Sub          | 38  |
| 3     | Sub2         | 39  |
| 4     | Open HiHat   | 40  |
| 5     | Snare        | 41  |
| 6     | Closed HiHat | 42  |
| 7     | Ride         | 43  |
| 8     | Clap         | 44  |

Tracks 7 & 8 are structurally identical to Tracks 1–6 with empty all-zero patterns (to be filled with ones later).

---

## MIDI OUTPUT

keep as is for now. 

---

## DRMlengths PRESETS — EXTENDED

Each preset extended with 2 additional values for Tracks 7 (ride) and 8 (clap): **16 in all 8 presets**.

---

## MORPHING

MorphX/Y affects **only the 16 CV sequences** (bilinear interpolation of 4 variants). Drum tracks have no morph involvement. 

---

## ROUTING PAGE (additions)

- Existing: Clock input (bus selector)
- New: **ShuffleClockIn** (bus selector) — second shuffled clock input
- New: **RecClock** (bus selector, 0 = none = recorder disabled)

---

## SHUFFLE PAGE (new)

8 ON/OFF parameters: Skick, Ssub, Ssub2, Soh, Ssnare, Sch, Sride, Sclap
- OFF → voice uses Clock input
- ON → voice uses ShuffleClockIn
- Shuffle clock affects drums only (CV and trigger sequences always use main clock for now)

---

## SEQUENCER PAGE (changes)

Add: DRMlengths, MorphX, MorphY, ZERO (moved from deleted DELTA page)
Remove: Seq Len 1–6

---

## LOOPER PAGE (new)

Parameters:
- **Record** (ON/OFF): starts recording on next RecClock rising edge when high; goes to playback when low
- **RecLength** (1–8 bars, default 4): 1 bar = 16 RecClock steps; switches to overdub at RecLength bars
- **Live!** (ON/OFF): rising edge → delete recording, return to live pass-through

Modes:
1. **Live/Pass-through** (initial state, after Live! trigger): no recording, all MIDI passes through
2. **Recording**: from first RecClock tick after Record goes high; records DIN MIDI stream
3. **Overdubbing**: after RecLength bars reached; loop plays back + new input passes and records
4. **Playback**: Record went low; loop masks incoming MIDI only where recorded (per-note, per-CC)

Buffer: 2048 MIDI events max. If cap reached before bar end, continue (recording empty) until planned end for correct loop length.

CC playback is independent of pattern selection — CC recordings continue even when pattern 1–16 is active.

---

## PATTERN 17

- Preset and Target selectors extended to 17
- Pattern 17 = recorded MIDI drum loop (drum buses only; CC recordings play independently always)
- **Crossfade with Pattern 17**: symmetric probability gating
  - Probability for source pattern drums = `(127 - Crossfade) / 127`
  - Probability for recording drums = `Crossfade / 127`
- If no recording exists and Pattern 17 selected: drum output silent; recorder controls active

---

## CH10 DRUM TRIGGERS

MIDI notes 1–8 on channel 10 (status byte 0x99) → 5ms high pulse on buses 37–44 respectively.
During recording, these are also captured in the loop. During playback, recorded ch10 notes retrigger buses AND go out on DIN+Internal (intended: drives both internal and external drum voices).

---

## TRIGGERS PAGE

Keep as-is. Add: mirror all output to Internal (`kNT_destinationInternal`) in addition to existing destination.
