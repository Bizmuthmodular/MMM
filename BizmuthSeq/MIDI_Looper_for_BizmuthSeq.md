# MIDILooper — standalone plugin, built incrementally from official examples

**Note:** This document formerly described a MIDI looper feature bolted onto BizmuthSeq. That
approach hit an unexplained module-freeze that extensive hardware bisection could not isolate;
BizmuthSeq has been rolled back to its last stable version (v15), and the looper is being rebuilt
from scratch as its own standalone plugin at `MIDILooper/MIDILooper.cpp`. This file now documents
that new plan. See `BizmuthSeq/History/BizmuthSeq_v17.cpp` for the abandoned in-BizmuthSeq attempt.

## Context

The MIDI looper was previously attempted as a feature bolted onto BizmuthSeq and hit an unexplained module-freeze that ~10 hardware-tested bisection rounds could not isolate to any single cause (SRAM size, DRAM allocation, `.bss` statics, and pointer-arithmetic patterns were all tried and ruled out). Rather than continue chasing that, BizmuthSeq has been rolled back to its last stable version (v15) and the looper is starting over as its own plugin, `MIDILooper/MIDILooper.cpp`.

This plan is based on three authoritative sources read directly for this session: `include/distingnt/api.h` (ground truth for every struct/function signature), `Disting NT Examples/distingNT_API/examples/nt_midi_router.cpp` (a complete, compact, official MIDI-only plugin — used for its clean single-file style, its 4-toggle destination pattern, and its "read `v[]` directly, no `parameterChanged()`" approach), and `Disting NT Examples/distingNT_API/examples/midilooper.cpp` (a real, more complex 8-track MIDI looper from the official examples — used for its memory architecture: DTC for frequently-touched global state, DRAM for the event buffer, and critically, a `construct()` that stays minimal and defers any "force parameters to a known state" work to a one-time guard inside `step()`, never calling `parameterChanged()` or `NT_setParameterFromAudio()` from `construct()` itself). The two local knowledge docs (`Local Disting NT Knowledge/*.md`) were also read and are consistent with `api.h`.

**Old approach is not being carried forward** — no `LooperState`-via-pointer indirection, no construct()-time loop calling `parameterChanged()` for every parameter, no manual pointer-arithmetic carving of one block into several typed sub-regions. Only the **feature set** (what the looper must do) carries over from the original design; **how** it's built is entirely re-derived from the two example files above.

## The feature set (the destination — not all built in stage 1)

- **ROUTING page**: RecClock bus selector (CV input, dedicated parameter — not reusing another parameter like the old design did), plus 4 independent MIDI destination toggles (Breakout / USB / Select Bus / Internal), matching `nt_midi_router.cpp`'s exact style.
- **LOOPER page**: `Record` (on/off), `RecLength` (1–8 bars), `Live!` (0→1 trigger) — same three parameters as the original BizmuthSeq Looper page.
- Modes: Live/pass-through → Recording → Overdub (auto after `RecLength` bars) → Playback (when Record turns off).
- Records **all** incoming MIDI regardless of physical port (API gives `midiMessage()` no source field — a hard constraint, not a choice).
- Playback-mode masking: a live note/CC is suppressed only if that exact (channel, number) identity was recorded; Overdub never masks.
- Sample-accurate event timing (store elapsed samples since the current RecClock step's rising edge).
- Buffer-full grace: stop capturing new events past the cap, keep the transport running so loop length stays correct.
- All-notes-off safety (CC 123 on all 16 channels) at record-stop→Playback and at Live! reset.
- `Live!` forces the `Record` parameter back to 0 via `NT_setParameterFromAudio`.
- Re-enabling `Record` from Playback resumes straight into Overdub (keeps the existing loop, no reset).

## Architecture (confirmed by the two examples + api.h)

- Small `_NT_algorithm`-derived struct in **SRAM** (`req.sram`) — just the base 4 fields plus whatever tiny scalars don't fit better elsewhere.
- Frequently-touched transport/loop state (mode, step counter, RecClock edge tracking, sample-since-edge counter) in a separate struct in **DTC** (`req.dtc`), following `midilooper.cpp`'s `MidiLooper_DTC` pattern — this is memory neither the BizmuthSeq attempt nor this session's bisection ever tried.
- The recorded-event ring buffer in **DRAM** (`req.dram`), a single flat `(LoopEvent*)ptrs.dram` cast — no carving into multiple sub-regions in stage 1 (only one buffer needed until playback-scheduling is added later).
- **No `parameterChanged()`** in stage 1 (`nullptr` in the factory, matching `nt_midi_router.cpp`) — every value read directly from `alg->v[...]` where needed, in `step()` and `midiMessage()`.
- **`construct()` stays minimal**: memset DTC, placement-new the algorithm struct in SRAM, set `parameters`/`parameterPages`, return. No forced parameter sync, no `NT_setParameterFromAudio` calls here.
- If any "sync internal state to current parameter values" step is ever needed, it happens once inside `step()` guarded by a `bool initialized` flag in DTC — exactly `midilooper.cpp`'s pattern — never inside `construct()`.
- Factory struct fully positional or fully designated (not mixed), matching `api.h`'s exact 23-field order.

## Stage 1 (this implementation pass) — scope

Everything above under "the feature set" is the eventual goal, but stage 1 only builds and hardware-verifies the **skeleton**, so a freeze (if one occurs) is caught immediately and isolated to a small diff:

1. Full parameter/page declarations: ROUTING page (RecClock CV input + 4 destination toggles) and LOOPER page (Record/RecLength/Live!) — all real, all visible in the UI. (Parameter/page declarations were already conclusively proven safe in the BizmuthSeq investigation, so declaring the full set now is low-risk.)
2. A real `step()` — reads the RecClock bus, does rising-edge detection, advances a step counter (0..16×RecLength−1) in DTC. No recording yet.
3. `midiMessage()` wired for real: straight pass-through of every incoming message to whichever of the 4 destinations are toggled on. No recording, no masking, no modes yet — just proves MIDI I/O works end-to-end in this new architecture.
4. No DRAM buffer yet, no `Record`/`Live!` behavior yet (the parameters exist and are visible, but nothing reacts to them).

## Critical files

- `MIDILooper/MIDILooper.cpp` — everything lands here (single file, following `nt_midi_router.cpp`'s compact style — no need for `midilooper.cpp`'s multi-header split since this is a single-track, much smaller feature set).
- `include/distingnt/api.h` — struct/function ground truth.
- `Disting NT Examples/distingNT_API/examples/nt_midi_router.cpp` — style/pattern reference.
- `Disting NT Examples/distingNT_API/examples/midilooper.cpp` — memory-architecture reference (DTC usage, construct()-time discipline).
- `Makefile` — add a `MIDILooper` build target alongside the existing `BizmuthSeq` one, or use `make cpp SOURCE=MIDILooper/MIDILooper.cpp OUT=MIDILooper/MIDILooper.o` directly.

## Verification

1. Compile: `make cpp SOURCE=MIDILooper/MIDILooper.cpp OUT=MIDILooper/MIDILooper.o`, zero errors/warnings under `-Wall`.
2. Parameter-count self-check (same convention as BizmuthSeq): counted `.name =` literals + macro-expanded I/O params must equal the enum's `NUM_PARAMS`.
3. Copy to `/programs/plug-ins/`, load on hardware, confirm no freeze and both pages show the right parameters.
4. Manual test: send MIDI in, confirm it passes through to whichever destination toggle(s) are on.
5. Only after stage 1 is confirmed stable on real hardware does the next increment (DRAM event buffer + actual recording) get planned in detail and built.
