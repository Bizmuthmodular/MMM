# Disting NT — Developer Reference

## What is the Disting NT?

The **Expert Sleepers disting NT** is a Eurorack module (22HP, 25mm deep) that runs a multi-algorithm signal processing engine. It can run up to 32 algorithms simultaneously, each of which can be a synthesizer, effect, utility, sequencer, sampler, or any custom plugin. Built-in algorithms are called "algos"; user-written code running in the same engine is called a "plugin" — functionally identical, different only in origin.

Power: 211mA (+12V) / 96mA (−12V) at idle. Does not use the +5V rail.

---

## Physical Controls

- **3 pots** (left, centre, right)
- **2 encoders** (left, right — both pushable)
- **4 pushbuttons** (1–4, left-to-right; configurable)
- **OLED display**
- **MicroSD slot** (front-accessible, FAT32 or exFAT)
- **USB-C** (High Speed USB 2.0 device — MIDI, Audio 2.0, and Video Class compliant; does not power from USB)

---

## Physical Inputs and Outputs

### Inputs — 12 total

- 12 input jacks on the left of the front panel, numbered 1–12
- All DC coupled, 3.5mm TS (mono — do **not** use TRS cables)
- Illuminated: red = positive voltage, blue = negative, purple = audio-rate
- **Two operating modes** (per rear jumper on J13):
  - **Modular level:** max ±12V, input impedance 101 kΩ
  - **Line level:** max ±3.5V (+10 dBu), input impedance 29 kΩ
- Inputs are also accessible on the J13 rear header

### Outputs — 8 total

- **6 output jacks** on the right of the front panel, numbered 1–6
- **2 additional outputs** on rear header J5 (pins 7–8); pins 1–6 of J5 mirror the front panel outputs
- All DC coupled, max range **±11V**

**Summary: 12 inputs, 8 outputs (6 front + 2 rear)**

---

## Virtual I/O — The Bus System

All signal routing uses **64 busses** internally.

| Bus range | Purpose |
|---|---|
| 1–12 | Driven directly from the 12 physical inputs |
| 13–20 | Feed the 8 physical outputs (at end of chain) |
| 21–64 | Aux busses for inter-algorithm routing (44 total; 8 visible in UI, all 44 in preset editor) |

- **Algorithms read from and write to busses.**
- Physical inputs → busses 1–12 (always at the top of the chain)
- Physical outputs ← the first 8 output busses (always at the end of the chain)
- Aux busses are free for any internal routing

---

## Signal Flow Rules

1. Algorithms execute in **strict top-to-bottom order** as listed in the algorithm overview.
2. An algorithm only sees bus states left by all algorithms **above** it.
3. Each algorithm output has an **Output Mode**:
   - **Add** — the algorithm's output is *summed* onto the existing bus contents (use for synths/sources mixing together)
   - **Replace** — the algorithm's output *overwrites* bus contents entirely (use for effects/inserts)
4. Multiple independent signal chains are possible — e.g., algo 1 on inputs 1–2 → outputs 1–2, algo 2 on inputs 3–4 → outputs 3–4 with no interaction.
5. A **Bypass** parameter is available on every algorithm: bypassed algorithms use zero CPU and pass nothing through (no signal passthrough at all).

---

## CV Mapping Rules

- CV mappings are **additive offsets** on top of the preset parameter base value.
- A CV of 0V leaves the parameter at its preset value.
- The math: `added value = (CV voltage) × Delta ÷ Volts`
  - "Volts" specifies how much CV is needed to move the parameter by "Delta"
- **Gate mode CV:** toggles the parameter between its min and max only.
- **Unipolar mode CV:** clamps negative voltages to 0V.
- **MIDI and I2C mappings** work differently — they **directly set** the parameter value (not additive).
- Multiple sources (CV, MIDI, I2C) can control the same parameter simultaneously.

### CV Mapping Sources

1. **Own inputs** — the busses connected to this algorithm's own inputs
2. **Module inputs** — the physical module input busses directly (1–12)
3. **Output of another algorithm** — any algorithm's output busses in the preset

---

## Parameter System

- Parameters are **16-bit signed integers** with defined min, max, and default values.
- Each parameter has:
  - **Min / Max / Default**
  - **Unit** (Hz, ms, dB, %, semitones, etc.)
  - **Type** (numeric, enum/list, string, special dB, special %)
- Parameters can be changed at any time with no audio interruption.
- **Specifications** differ from parameters: set at algorithm instantiation time; changing them interrupts audio and rebuilds the preset. They typically control memory allocation (e.g., max delay time) or parameter count (e.g., number of mixer channels).

---

## Algorithm / Plugin Structure

### Built-in Algorithms
- Up to **32 algorithms** per preset.
- Each algorithm has a **4-byte GUID**.
- Each algorithm can have up to 3 **specifications** (set at add time via the menu).
- Algorithms can be renamed (custom name shown in the overview).
- CPU usage is shown per-algorithm and as a total percentage in the overview.

### Plugins (User-Written C++)

- Plugins are **compiled C++ object files (`.o`)** placed on the MicroSD card at `/programs/plug-ins/`.
- Plugins define new algorithms — they appear after built-in algorithms in the Add Algorithm list.
- Plugins are scanned at startup (and on card remount).
- **There is no UI mechanism to unload plugins** — only reboot. For development iteration, use the SysEx rescan command (7AH/08H).
- The plugin API is at: https://github.com/expertsleepersltd/distingNT_API
- A **Faust architecture** is available for compiling Faust DSP programs to disting NT plugins: https://github.com/expertsleepersltd/distingNT_API/tree/main/faust

### Lua Script Algorithm
- Lua scripts (`.lua`) go in `/programs/lua/` on the MicroSD card.
- The "Lua Script" algorithm type runs these scripts as processing algorithms.
- A simpler "Lua Expression" algorithm is also available.
- Lua scripting documentation is a separate document downloadable from: https://expert-sleepers.co.uk/distingNTfirmwareupdates.html
- **Development workflow:** use SysEx `08H` (Execute Lua) / `09H` (Install Lua) to iterate without copying files to the card every time.

### UI Scripts
- Stored in `/ui_scripts/` (`.lua`).
- Completely redefine the module UI (all 4 buttons, 2 encoders, 3 pots, display).
- Used for custom live performance UIs.
- Always exit by pressing all four pushbuttons simultaneously.

---

## Sample Rate, Buffer Size, CPU

| Setting | Default | Options |
|---|---|---|
| Sample rate | 48 kHz | 32, 44.1, 48, 88.2, 96 kHz |
| Buffer size | 24 frames | Selectable |
| Control rate | 2 kHz (= 48000 ÷ 24) | — |

- Higher sample rate or smaller buffer → higher CPU load.
- **Target: stay below 90% CPU total.**
- At CPU overload: the module mutes all outputs and displays "CPU OVERLOAD !MUTED!" until an algorithm is removed, bypassed, or respecified, or a new preset is loaded.

---

## MIDI

### Physical MIDI Connections
- **Breakout header J10** (4-pin, same pinout as disting mk4/EX/FH-2/ES-9/CVM-8): supports TRS Type A via Tiny MIDI Breakout accessory
- **Select Bus**: MIDI piggybacked on the CV power bus line; no extra cables needed between modules on the same bus
- **USB MIDI**: class compliant, no driver needed on macOS/iOS/Linux; Windows driver available

### MIDI Mappings
- Map **MIDI CCs, channel pressure (aftertouch), or notes** to any parameter.
- MIDI mappings **directly set** parameter values (not additive).
- Supported CC types: standard 0–127, 14-bit CC pairs, relative CC
- Note types: momentary (note on = Max, off = Min) or toggle
- Symmetric mode: maps CC value 64 to parameter midpoint (for bipolar parameters)
- Soft takeover modes: Sudden, Gradual, Always On
- Module can send CCs when parameters change (configurable)
- Program Change loads presets by filename `#N` pattern or alphabetical index

---

## I2C

- Disting NT acts as **I2C follower** (not leader).
- I2C address configurable in Settings.
- Voltage/pitch scaling: **16384 = 10V** (signed 16-bit)
- Velocities: 0–16384
- Parameter/algorithm/preset numbers: **1-based**

### Key I2C Commands

| Hex | Description |
|---|---|
| 0x11 | Set controller X to value Y (for CV mapping) |
| 0x46 | Set parameter X to value Y (actual range); param 255 = set target algorithm index |
| 0x47 | Set parameter X to value Y (0–16384 scaled to parameter's range) |
| 0x48 | Get parameter value |
| 0x49 | Get parameter min |
| 0x4A | Get parameter max |
| 0x54 | Set pitch for note ID |
| 0x55 | Note on for note ID (velocity MSB+LSB) |
| 0x56 | Note off for note ID |
| 0x57 | All notes off |
| 0x68 | Set pitch with channel |
| 0x69 | Note on with channel |
| 0x6A | Note off with channel |
| 0x6B | Set channel for subsequent note commands |
| 0x4F | Send MIDI from breakout |
| 0x50 | Send MIDI from Select Bus |
| 0x81/82/83 | Send MIDI to disting NT (1, 2, or 3 bytes) |

---

## SysEx Reference (MIDI)

All SysEx messages use header: `F0 00 21 27 6D <SysEx ID>` then command byte.

Key commands relevant to development:

| Command | Hex | Description |
|---|---|---|
| Execute Lua | 08H | Run arbitrary Lua chunk, get result back |
| Install Lua | 09H | Install Lua script into a Lua Script algorithm instance |
| Get algorithm list | 31H | Returns GUID, name, specs, plugin flag, filename |
| Add algorithm | 32H | Add algorithm by GUID |
| Remove algorithm | 33H | |
| Load preset | 34H | |
| New preset | 35H | |
| Save preset | 36H | |
| Move algorithm | 38H | Reorder algorithms |
| Get parameter value | 45H | |
| Set parameter value | 46H | |
| Get parameter info | 48H | Units string |
| Get enum strings | 49H | |
| Set string parameter | 53H | |
| Get/set preset paths | 56H | |
| MicroSD sub-protocol | 7AH | Directory list, upload, download, delete, rename, rescan plug-ins (08H sub-cmd) |
| Reboot | 7FH | |
| Bootloader mode | 7F 7FH | |

---

## MicroSD File Structure

```
/programs/plug-ins/     → C++ plugin .o files
/programs/lua/          → Lua scripts for Lua Script algorithm (.lua)
/programs/three_pot/    → Three Pot algorithm programs (.3pot)
/ui_scripts/            → Lua UI scripts (.lua)
/presets/               → JSON preset files (default save/load location)
/samples/<subfolder>/   → WAV audio samples
/wavetables/            → Wavetables (single WAV or subfolders of single-cycle WAVs)
/FMSYX/                 → Yamaha DX7 SysEx banks (.syx, exactly 4104 bytes each)
/scl/                   → Scala scale files (.scl)
/kbm/                   → Scala keyboard map files (.kbm)
/MTS/                   → MIDI Tuning Standard SysEx dumps (.syx, exactly 408 bytes)
/MIDI/<subfolder>/      → MIDI song files (.mid)
/recordings/            → Audio recorder output (configurable path)
```

### Card Limits
- DX7 FM banks: up to 100
- .scl files: up to 1000; .kbm files: up to 1000; MTS dumps: up to 1000
- Sample folders: up to 1000; total samples: up to 10,000; files per folder: up to 1000
- Wavetables: up to 1000
- MIDI folders: up to 1000; files per folder: up to 1000

---

## Polysynth Features (Relevant for Synth Plugin Development)

- Polysynths can use **up to 6 input busses as gates**; each gate can have **up to 11 CV busses** immediately following it.
- **Gate velocity:** gate voltage = velocity; 5V = maximum velocity.
- **Min/Max note** parameters filter the MIDI note range (also applies to I2C and CV/gate input, not just MIDI).
- **MPE support:** per-note pitch bend, aftertouch, CC#74 ("Y dimension"); common channel + note channels configurable.
- **Microtuning:** all polysynths support Scala (.scl + optional .kbm) and MTS (live SysEx or .syx file).
- **Sustain modes:** "Synth" (no retrigger) or "Piano" (retrigger allowed).

---

## Preset Format

- JSON, human-readable, stored on MicroSD.
- Contains: algorithm list (with GUIDs), all parameter values, all mappings (CV, MIDI, I2C).
- Default save/load location: `/presets/` (configurable).
- **End-of-Chain (EoC):** a marker that splits the algorithm list; algorithms after it persist across preset loads. Used for permanent final-stage processing (e.g., limiter, EQ).

---

## Key Numbers at a Glance

| Item | Value |
|---|---|
| Physical inputs | 12 |
| Physical outputs | 8 (6 front + 2 rear) |
| Total busses | 64 |
| Input busses (physical) | 12 (busses 1–12) |
| Output busses (physical) | 8 |
| Aux busses | 44 (8 visible in UI) |
| Max algorithms per preset | 32 |
| Default sample rate | 48 kHz |
| Default buffer size | 24 frames |
| Control rate | 2 kHz |
| Modular input max voltage | ±12V |
| Modular input impedance | 101 kΩ |
| Line input max voltage | ±3.5V (+10 dBu) |
| Line input impedance | 29 kΩ |
| Output max range | ±11V |
| Gate: 5V | = max velocity |
| I2C voltage scaling | 16384 = 10V |

---

## Developer Tools & Resources

| Resource | Location |
|---|---|
| C++ Plugin API | https://github.com/expertsleepersltd/distingNT_API |
| Faust architecture | https://github.com/expertsleepersltd/distingNT_API/tree/main/faust |
| Lua scripting docs | https://expert-sleepers.co.uk/distingNTfirmwareupdates.html |
| Presets / tools / flash scripts | https://github.com/expertsleepersltd/distingNT |
| Preset editor (web, Chrome only) | `dnt_preset_editor.html` from firmware releases zip (uses Web MIDI API) |
| NT Helper app (macOS/Win/Linux/iOS/Android) | https://nosuch.dev/nt-helper/ |
| NT Gallery (plugin database) | https://nt-gallery.nosuch.dev |
| SysEx browser tools | https://github.com/expertsleepersltd/distingNT/tree/main/tools |
| Expert Sleepers SysEx ID | `00H 21H 27H`; disting NT product byte: `6DH` |
