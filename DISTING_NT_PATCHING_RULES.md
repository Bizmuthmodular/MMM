# Disting NT — Patching Rulebook

**Scope:** this document is about building and live-editing *patches* (presets) — choosing which algorithms to add, in what order, and how to route them via the bus system — including the behaviour of an AI-aided helper process that stays connected over USB and edits routing on an ongoing basis. It is **not** about writing plugin C++ code (see `CLAUDE.md` for that). It is written to be used as a rule system by an AI helper that turns a text prompt ("make me a plucky bass patch", "build a kick + hat + clock generator") into a concrete list of algorithms, their slot order, and their bus routing, and that keeps editing/refining that routing live.

Source: Expert Sleepers disting NT user manual (v1.15) — "Signal flow" and "Algorithms menu" sections — plus the module's documented bus/CV/MIDI behaviour, plus hardware-specific facts about this particular rig (the 8-output physical expander, §2).

---

## 1. What a Patch Actually Is

A patch (preset) is:
1. An **ordered list of algorithm instances** (max **40** per preset, as of firmware 1.16.0). Order = position in this list = "slot".
2. Each algorithm instance has its own **parameter values** (including which bus(es) it reads/writes).
3. Optionally, **CV/MIDI/I2C mappings** layered on top of parameters.
4. Optionally, an **End of Chain (EoC)** split point (see §9).

There is no separate "wiring" data structure — routing *is* parameter values (bus-selector parameters) on each algorithm. Building a patch = choosing algorithms, choosing their slot order, and setting their bus-selector parameters correctly.

---

## 2. The Bus System

**64 busses total, numbered 1–64 (always 1-based).** A bus is a mono signal path carrying one float value per sample (volts).

| Bus range | Count | Purpose |
|---|---|---|
| 1–12 | 12 | Physical inputs. Bus *N* always equals hardware input jack *N*. |
| 13–20 | 8 | Physical outputs 1–8 (on-module: 6 front + 2 rear header). |
| 21–28 | 8 | **On this rig: physical outputs 9–16**, via an 8-output physical expander module wired to "Aux 1–8." See hardware note below. |
| 29–64 | 36 | True free/general-purpose aux busses for inter-algorithm routing on this rig. |

### Hardware note: this rig has an 8-output expander
This disting NT is fitted with an **8-output physical expander module**. The disting NT's own 8 documented outputs (busses 13–20) are followed by 8 more real output jacks on the expander, which are physically wired to what the stock manual calls "Aux 1–8" (busses 21–28, the first 8 aux busses — the same ones visible live on the hardware display).

**Consequence: on this system, busses 21–28 are not free scratch busses.** They carry real, physical, audible/patchable signal to jacks 9–16. Treat them exactly like 13–20: reserved for deliberately-chosen final destinations, never auto-allocated as internal scratch wiring. Only **busses 29–64** are genuine free aux/scratch busses on this rig.

(The stock/generic disting NT — without this expander — treats all of 21–64 as 44 general-purpose aux busses. That generic fact is true of the hardware in isolation, but does not apply to this patched-together rig; always use the corrected ranges above when generating or editing patches here.)

- **Bus 0 = "None" / disconnected.** Used for CV-input parameters that default to "not mapped."
- Busses 1–12 and 13–28 are **fixed to physical jacks** on this rig — never reassign them as if they were free.
- Busses 1–12 are read-only in effect: only physical hardware drives them; don't route an algorithm's output there.
- If a patch's final signal isn't written to 13–20 or 21–28 (this rig), it never reaches a real output jack.

### Voltage convention (for reference when picking gain/level parameters)
- Audio: ~±5V typical, never exceed ±11V (hardware output ceiling).
- Unipolar CV / envelopes: 0–10V.
- Bipolar CV / LFO: ±5V.
- Pitch CV: 1V/octave, 0V = C4 (261.6 Hz) by convention.
- Gate/trigger: 0V = off, 5V = on/max velocity.

---

## 3. Signal Flow: Slot Position Determines Everything

1. Algorithms execute in **strict top-to-bottom order**, exactly as listed in the algorithm overview (their slot order in the preset).
2. **An algorithm can only see bus contents left behind by algorithms above it in the list.** An algorithm below never affects one above it, no matter what bus it writes to.
3. At the very start of processing a block: busses 1–12 already hold the live physical input samples. Busses 13–64 (outputs + aux) start at **zero** for the first algorithm in the list, then accumulate/change as each algorithm in turn reads and writes them.
4. Consequence: **if algorithm B is meant to process algorithm A's output, B must be placed below A in the list**, and B's input bus parameter must equal A's output bus parameter.
5. Consequence: **order alone can break a patch even if bus numbers are "correct."** Assigning the right bus number to the wrong slot position produces silence or stale/zero data.
6. **Bypass** on an algorithm = zero CPU, and the algorithm passes nothing through at all — it does not act as a wire.

### Independent, parallel chains are normal and encouraged
Nothing requires one single serial chain from input to output. Example: algorithm 1 reads inputs 1–2 → writes outputs 1–2; algorithm 2 (elsewhere in the list) reads inputs 3–4 → writes outputs 3–4. These are two fully independent virtual modules coexisting in one preset.

### Reordering
The user-facing operations are **Move algorithm up** / **Move algorithm down** (one slot at a time) and **insert at current position** (vs. append at end) when adding a new algorithm. State algorithms in their final intended top-to-bottom order — that order *is* the signal flow graph. See §10 for the rules governing when the helper is allowed to reorder slots on its own initiative.

---

## 4. Output Mode: Add vs Replace

Most algorithm outputs expose an **Output Mode** parameter: `Add` or `Replace`.

| Mode | Behaviour | Use for |
|---|---|---|
| **Add** (0) | `bus[i] += algorithm_output[i]` — sums onto whatever is already on the bus | Sound **sources**/instruments (so multiple instruments feeding the same bus mix together audibly), and effects used as a **send** |
| **Replace** (1) | `bus[i] = algorithm_output[i]` — overwrites the bus entirely, destroying whatever was there | Effects used as an **insert** (EQ, compressor, filter, waveshaper), and anything that must fully own its output bus |

Rules of thumb:
- **Two or more instruments/sources sharing one bus → both must be `Add`.** If either is `Replace`, it silently erases the other.
- **An insert effect reading and writing the *same* bus → `Replace`.** `Add` here would double the signal instead of replacing it.
- **A send effect tapped off to a shared mix bus → `Add`** onto the destination.
- If nothing else touches a bus, prefer `Replace` as the clearer default; use `Add` only when summing is the actual intent.

---

## 5. Procedure: Turning a Prompt Into a Routed Patch

1. **Decompose the request into a signal graph on paper first**: functional blocks and the direction signal must flow between them, independent of bus mechanics.
2. **Pick physical I/O.** Anything receiving a real-world signal reads from 1–12. Anything that is the final sound/CV writes to 13–20 or 21–28 (this rig's expander outputs 9–16), per the user's stated jack.
3. **Assign one aux bus (29–64 on this rig) per internal signal edge.** Don't reuse an aux bus for two unrelated edges unless the intent is genuinely to mix/replace onto the same edge.
4. **Topologically sort the graph** (sources → processors → sinks) and lay that out as the slot list top-to-bottom.
5. **For each output, decide Add vs Replace** using §4.
6. **Sanity-check for collisions**: no bus written by two algorithms unless both are intentionally `Add`.
7. **Sanity-check for orphans**: every allocated aux bus must be both written and read.
8. Confirm at least one algorithm terminates on 13–20 or 21–28 with the correct Output Mode for that jack.

---

## 6. Bus Allocation Conventions

- Reserve **1–12** exclusively for real input jacks.
- Reserve **13–20** exclusively for final signal on this module's own output jacks 1–8.
- Reserve **21–28** exclusively for final signal on the expander's output jacks 9–16 (this rig — see §2). Do not use for internal scratch.
- Use **29–64** for everything internal on this rig.
- **Use as few busses as possible overall.** Prefer retiring and reusing an aux bus once its signal has been fully consumed over allocating a fresh one — but never let two still-live, unrelated signal edges share a bus (see collision rule, §5.6).
- Never invent a 65th bus or a 0-based bus number for routing — bus 0 exclusively means "disconnected."

---

## 7. CV / MIDI / I2C Mapping Rules

- **CV mappings are additive offsets**, not replacements: `added value = (CV voltage) × Delta ÷ Volts`. A CV of 0V (or unmapped) always leaves the parameter at its plain preset value.
- **Gate-mode CV mapping**: toggles the parameter only between its min and max.
- **Unipolar-mode CV mapping**: clamps negative voltage to 0V before applying.
- **MIDI and I2C mappings directly set** the parameter value — not additive, and they override the additive CV baseline.
- Multiple mapping sources (CV + MIDI + I2C) can drive the same parameter simultaneously; effects combine.
- **Three CV source choices** per mapping: `Own inputs` (default — this algorithm's own input busses), `Module inputs` (physical busses 1–12 directly), or the **output of another algorithm** (survives reordering/deletion better than relying on `Own inputs` pointed at whatever happens to precede it).

---

## 8. Constraints & Limits Reference

| Item | Value |
|---|---|
| Total busses | 64 (1–64, 1-based) |
| Physical input busses | 12 (busses 1–12) |
| Physical output busses, stock module | 8 (busses 13–20) |
| Physical output busses, **this rig (with expander)** | **16** (busses 13–20 + 21–28) |
| Free/general aux busses, **this rig** | **36** (busses 29–64) |
| Free/general aux busses, stock module (no expander) | 44 (busses 21–64) |
| Max algorithms per preset | 40 (firmware 1.16.0+) |
| Bus 0 | Reserved meaning: "None / disconnected" |
| Output modes | Add (sum) / Replace (overwrite) |
| Default sample rate | 48 kHz (selectable: 32/44.1/48/88.2/96 kHz) |
| Default buffer size | 24 frames → 2 kHz control rate |
| CPU budget | Stay below ~90% total; overload mutes all outputs |
| Polysynth gate/CV grouping | Up to 6 input busses as gates; each gate can have up to 11 CV busses immediately following it |

---

## 9. End of Chain (EoC)

EoC is a marker splitting the algorithm list: everything **below** the marker is preserved unchanged when a different preset is loaded. Use this when a prompt implies a **permanent final-stage utility** that should survive preset switching (e.g. a master limiter/EQ). Place that algorithm at the end of the list and note that the user must set the EoC marker above it via the module UI.

---

## 10. Refactoring Freedom: What Must Be Preserved vs. What's Free to Change

The helper should actively try to **use as few busses as possible**, and is allowed to **move algorithms up or down** in slot order to make that possible. It is **not** allowed to break existing routing while doing so.

**What matters (must be preserved across any refactor):** the *connection graph* — which algorithm's output feeds which algorithm's input, and with what Add/Replace semantics reaching that destination. This is the real patch; everything else is implementation detail.

**What doesn't matter (free to change):** exact slot positions and exact bus numbers used to implement a given connection today. The helper may renumber busses and reorder slots however it likes in service of minimizing bus count, provided that:

1. **Ordering is re-verified after any reorder** — every writer of a connection must remain above every reader of that same connection (§3). A reorder that shrinks bus count can just as easily introduce a new ordering violation if done carelessly; re-run the §5 collision/orphan checks after any refactor.
2. **The effective signal reaching each input is unchanged** — same logical source, same Add/Replace outcome, just possibly a different bus number or slot index carrying it.
3. **Physical I/O busses are never reassigned away from their fixed meaning** — 1–12, 13–20, and (on this rig) 21–28 keep their hardware meaning regardless of refactoring.
4. **A bus number may only be reused once its previous connection is fully "closed"** — i.e. no remaining reader anywhere below the point of reuse still expects the old signal on that bus number. Reusing a bus while a downstream reader still depends on the old signal on it is a routing break, not a minimization.

---

## 11. AI-Aided Live Patching: Mandatory Archive Before Save

An AI-aided helper process may stay connected to the module continuously over USB, live-editing algorithms, routing, and parameters as it works.

**Hard rule: such a process is never allowed to save/persist a patch change back to the module unless it has first archived the current patch to a folder on the Mac running the disting NT helper app** — not onto the disting NT itself or its MicroSD card — **named "DistingNT Patching Rules."**

- Archive-then-save is mandatory, not best-effort: no save operation may proceed without a preceding successful archive write.
- The archive target is the Mac's filesystem (the machine running the helper app), never the module's own storage.
- Purpose: guarantees an external, recoverable version history of every patch state the AI has produced, before any change becomes persistent on the module.

---

## 12. Common Mistakes to Avoid

- **Wrong slot order**: placing an effect above the source it's meant to process.
- **Replace stepping on Add (or vice versa)**: two sources sharing a bus where one is `Replace` silently kills the other; an insert effect left on `Add` doubles its input instead of replacing it.
- **Aux bus reuse across unrelated, still-live signal edges**: causes unintended summing/overwriting (see §10.4 for when reuse is actually safe).
- **Orphaned aux bus**: allocated but only written, or only read.
- **Writing to busses 1–12**: pointless — nothing reads them for anything but the physical input jack.
- **Treating busses 21–28 as free scratch on this rig**: they are the 8-output expander's physical jacks 9–16 (§2), not general aux.
- **Expecting Bypass to act as a pass-through wire**: it doesn't — bypass = zero signal.
- **Forgetting CV mapping is additive**: a patch needing CV to *set* (not offset) a value needs a MIDI/I2C mapping instead, or a base value/Delta chosen so the CV's range reaches the desired outcome.
- **Exceeding 40 algorithms** (firmware 1.16.0+ limit), or assuming free reassignment of busses 1–12 / 13–20 / 21–28.
- **Saving a live AI-driven change without first archiving it** to "DistingNT Patching Rules" on the Mac (§11) — never optional.
