# MIDILooper — Current State & Learnings

`MIDILooper/MIDILooper.cpp` — a standalone disting NT plugin. GUID `NT_MULTICHAR('M','I','D','L')`,
tag `kNT_tagUtility`. A MIDI-clock-synced looper: records incoming MIDI (notes + CCs), plays it
back in sync with an external clock, and drives LED feedback + accepts control input from
physical controllers via hardcoded MIDI note/channel assignments.

---

## Why this is a separate plugin (origin story / key learning)

MIDILooper was originally attempted as a feature bolted onto `BizmuthSeq`, a drum/CC sequencer
plugin. That attempt caused the module to freeze on load, and an extensive same-session
bisection (roughly a dozen hardware-tested iterations) never conclusively identified the root
cause — theories tested and ruled out included SRAM struct size, DRAM allocation, `.bss` static
arrays, and manual pointer-arithmetic buffer carving. `BizmuthSeq` was rolled back to its last
stable version, and the looper was rebuilt from scratch as this independent plugin, deliberately
following patterns from two official Expert Sleepers examples rather than the approach that had
caused trouble:

- `Disting NT Examples/distingNT_API/examples/nt_midi_router.cpp` — compact single-file style,
  reading `alg->v[]` directly with **no `parameterChanged()`**, and the 4-toggle MIDI destination
  pattern.
- `Disting NT Examples/distingNT_API/examples/midilooper.cpp` — a real multi-track MIDI looper
  from Expert Sleepers' own examples. Its header dependencies (`recording.h`, `playback.h`, etc.)
  aren't present in this repo, so only its top-level `.cpp` was available to study — but it
  confirmed the pattern of using **DTC memory** for frequently-touched per-sample state (rather
  than the SRAM instance struct) and keeping `construct()` deliberately minimal: no
  `parameterChanged()` calls, no `NT_setParameterFromAudio()` calls from inside it.

**The practical takeaway carried into every design decision below**: keep the SRAM-resident
algorithm struct as small as possible, put everything else in DTC (or DRAM for genuinely large
buffers), never call `parameterChanged()` from `construct()`, and read parameter values directly
wherever they're needed instead of caching them via a `parameterChanged()` callback. This
plugin has never frozen on load once that discipline was adopted — but the exact mechanism
behind the original `BizmuthSeq` freeze was never identified, only avoided.

---

## Architecture

- **SRAM** (`MIDILooperAlgorithm : public _NT_algorithm`): just the base 4 pointers plus a `dtc`
  pointer and a `loopBuffer` pointer. Deliberately tiny.
- **DTC** (`MIDILooper_DTC`): everything touched every `step()`/`midiMessage()` call — transport
  mode, loop position, edge-detection state, the pending-event pools, the quantizer hold pool,
  the active-note bitset. Currently 484 bytes.
- **DRAM**: the recorded-event ring buffer (`LoopEvent loopBuffer[]`, 2048 entries × 8 bytes =
  16 KB), a single flat cast with no sub-region carving.
- **No `parameterChanged()`** anywhere in the factory — every parameter is read directly from
  `alg->v[...]` at the point of use, in `step()` or `midiMessage()`.
- Parameter/page declarations use small `constexpr` helper functions (`makeParameter()`,
  `makePage()`) matching `nt_midi_router.cpp`'s style, rather than raw designated-initializer
  literals.

---

## Pages & parameters (current)

**Routing**
- `RecClock` — CV input. The high-resolution 1/16-step clock: drives `loopStep` advancement,
  the Recording→Overdub auto-transition, and playback scheduling. This role has been stable
  since early in the plugin's life and is treated as sacrosanct — later features were
  deliberately built *around* it rather than changing its behavior.
- `Sync` — CV input. Added later to take over **only** "when does a fresh recording actually
  start" — `Record` turning on from Live no longer starts capturing immediately; it arms and
  waits for the next `Sync` rising edge, which resets `loopStep` to 0 and begins capture exactly
  on that edge. Resuming into Overdub from Playback still starts immediately (no Sync wait),
  since that's continuing an already-aligned loop, not starting a fresh one.
- `Breakout` / `USB` / `Select Bus` / `Internal` — four independent on/off destination toggles
  (`nt_midi_router.cpp`'s multi-toggle style), not a single destination enum.

**Looper**
- `Record` — on/off.
- `RecLength` — **enum**, not a raw integer: `1/4, 1/2, 1, 2, 4, 8, 16` (bars), each mapping to a
  step count (`kRecLengthSteps`: 4/8/16/32/64/128/256). The loop tracks two lengths separately:
  `activeLenSteps` (what `loopStep` currently wraps against) and `recordedLenSteps` (how much has
  actually been captured, locked in the moment Recording completes one full lap). Shrinking the
  requested length takes effect immediately, truncating from the end; growing only takes effect
  immediately up to `recordedLenSteps` — a request beyond that just stays pending for the next
  fresh take. Shrinking never deletes anything from the buffer, so growing back restores the
  previously-hidden tail automatically.
- `Live!` — 0→1 trigger: clears the recording, returns to Live/pass-through, forces `Record` back
  to 0 via `NT_setParameterFromAudio()` so the module's own display reflects it.

**Quantizer**
- `Quant2Clk` — on/off. Off: no change from normal timing. On: every note is quantized to the
  nearest `RecClock` edge (using the previous inter-edge interval's measured length as the
  estimate for the current one) — a note closer to the edge that just happened processes
  immediately with its timing snapped to 0; a note closer to the upcoming edge is held
  (`quantHold[]` pool) and released exactly on that edge. Applies uniformly to captured timing
  and live pass-through (a live note is never heard except in sync with a clock edge). Switching
  back to Off immediately flushes anything currently held rather than stranding it.

---

## Modes

`Live → Recording → Overdub (auto after one full RecClock lap) → Playback (Record off)`, with
`Record` turning back on from Playback resuming straight into Overdub on the existing loop.
`Live!` (or the hardcoded loop-button note, see below) resets to Live from any state.

Overdub capture **overwrites in place** rather than always appending: a new note/CC at the same
step with the same (channel, identity) as an already-recorded one replaces that entry instead of
growing the buffer — this is what lets repeated overdub passes on the same material not exhaust
the 2048-event cap; only genuinely new content consumes a fresh slot.

---

## Internal MIDI feedback prevention

`kNT_destinationInternal` is a same-unit software bus with no per-message source information —
anything the plugin sends via Internal echoes straight back into its own `midiMessage()`, with
no way to tell it apart from new input. Fixed with a **content/echo channel partition**
(`kChannelRemaps`): channels actually used (1, 2, 3, 4, 6, 10, 13) each map to their own
dedicated, otherwise-unused echo channel (see the table below for the exact current pairing),
and any incoming message on one of those echo channels is unconditionally dropped before any
other processing. Channel 5 has no Internal-echo mapping — it was given up to
free channel 14 for the hardcoded record-button input (see below); all 16 MIDI channels were
already allocated across the two sets, so there was no spare channel to reassign it to.

Current table (0-indexed internally; shown here as MIDI channel numbers):

| Content channel | → Internal echo channel |
|---|---|
| 1 | 7 |
| 2 | 8 |
| 3 | 9 |
| 4 | 12 |
| 6 | 15 |
| 10 (drum triggers) | 11 |
| 13 | 16 |

Channels not in this table are simply never forwarded to Internal at all (withheld), and are not
filtered on input either.

---

## Hardcoded controller integration

Two physical controllers are part of this setup: a **Vestax VCM-600** and an **Intech Studio
Grid**. The Internal-routing channel set above (channels 1, 2, 3, 4, 6, and 13 in particular)
exists because this plugin needs to pass controller-originated data through to another algorithm
on the same disting NT unit without feedback — very likely corresponding to the Intech Grid's
individually-addressable cells (each assignable to its own MIDI channel), though the exact
per-channel device assignment was never explicitly confirmed in the sessions that built this and
is worth documenting directly against the actual controller configuration if precision matters
later.

The following specific note/channel assignments are hardcoded in `MIDILooper.cpp` and match the
kind of dual-role pad (one physical button/pad that both sends a note on press *and* has its LED
driven by the same note number) common on grid-style controllers — consistent with the VCM-600's
button+LED layout:

| Channel | Note | Direction | Role |
|---|---|---|---|
| 13 | 83 (`kLedNoteRecord`) | Out | Sustained Note On while Recording or Overdub — drives the record LED. Hardcoded straight to DIN (`kNT_destinationBreakout`), bypassing the configurable destination toggles. |
| 13 | 83 | In | Intercepted and discarded (both Note On and Off) — protects the recording from the button's own press/release leaking in as captured content. **No function is wired to this input** — it only guards recording integrity. |
| 13 | 84 (`kLedNoteLoop`) | Out | Sustained Note On while Overdub or Playback — drives the loop LED. Same DIN-only hardcoding as note 83. |
| 13 | 84 | In | **Not currently intercepted.** Unlike note 83, nothing protects this input from being captured/passed through as musical content if the same physical pad also sends it on press. Flagged as a known gap, not yet addressed. |
| 13 | 89 (`kRecLengthIncNote`) | In | Note On only steps `RecLength` up by one enum index (clamped at "16"), via `NT_setParameterFromAudio()`. Intercepted, never recorded/passed through. |
| 13 | 90 (`kRecLengthDecNote`) | In | Note On only steps `RecLength` down by one enum index (clamped at "1/4"). Same interception. |
| 10 | any | In/Out | Drum trigger channel — remapped to channel 11 when routed to Internal (see table above), to trigger a drum-sound-generating algorithm on the same disting NT unit. |

**Note on channel 14**: it was originally going to carry a dedicated `Record` start/stop control
note (separate from the ch13 LED channel, to avoid exactly the note-83-input-collision problem
described above) — that specific feature was implemented once, then explicitly undone at the
user's request in the same session. Channel 14 was subsequently freed from the Internal-echo
table for this purpose but no input handler currently exists there; it's available if that
control scheme is wanted again.

---

## Known gaps / open items

- **Ch13 note 84 input is unprotected** (see table above) — the same class of bug that was fixed
  for note 83 has not been applied to note 84 yet.
- **No masking in Playback mode**: a live note that duplicates something already looping will
  currently sound twice (once live, once from the recorded playback) — the original design
  called for masking recorded-identity notes during pure Playback, never implemented in this
  plugin.
- **Exact VCM-600 / Intech Grid channel assignments beyond what's hardcoded above** were
  described conversationally during development rather than fully documented per-device; worth
  cross-checking against the actual controller configs if this file is used as a future reference.
- **The original BizmuthSeq freeze root cause was never identified** — only avoided by this
  plugin's architecture. If a similar freeze ever recurs here, the SRAM/DTC/`construct()`
  discipline described above is the first thing to re-verify, not assume is safely load-bearing
  forever.

---

## Build

Standalone `.cpp`, no dependencies beyond `distingnt/api.h`. Compile with:

```
make cpp SOURCE=MIDILooper/MIDILooper.cpp OUT=MIDILooper/MIDILooper.o
```

No version history subfolder has been kept for this plugin yet (unlike `BizmuthSeq/History/`) —
worth starting one if further major changes are made, per this project's usual convention.
