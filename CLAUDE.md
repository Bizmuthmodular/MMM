# Disting NT — Project Knowledge Base

---

## HOW CLAUDE MUST USE THIS FILE

- **Rely on this file and your own C++ programming knowledge. Do not perform web searches.**
- All disting NT facts come from this file. If something is not described here, say so clearly rather than inferring or guessing.
- **The goal is to develop truly functioning disting NT plugins.** Do not suggest approaches, APIs, or techniques that are not supported by this file or the actual API headers in `include/distingnt/`. If request cannoa t be fulfilled with what is documented here, say so directly. Do not invent plausible-sounding solutions to satisfy a request.

---

## WORKFLOW RULE (set 2026-10-24, overrides older "write a test harness" guidance)

- **For the 4x4 plugin, the ONLY deliverables are `4x4/4x4.cpp` and the compiled `4x4/4x4.o`.** Put new feature code in `4x4.cpp`. Do not create any other files — in particular **do NOT write tester / desktop-harness `.cpp` files (`test_*.cpp`, `verify_*.cpp`, `diag_*.cpp`) and do NOT run internal desktop compiles/tests to "validate" behavior.**
- The only real test is the user running the compiled `.o` on the hardware. After writing `4x4.cpp`, cross-compile it to `4x4.o` (that ARM compile is the one allowed build — it catches compile errors and produces the deliverable) and hand it over for on-device testing. Do not fabricate or imply any other verification.
- Existing `.h` files may be read and, only if strictly necessary, edited; prefer keeping new code in `4x4.cpp`. Never add new headers or test files.

---

## Plugin File and Folder Structure

Every plugin has a name. Each plugin lives in its own folder in the project root, named after the plugin.

```
/PluginName/
    PluginName.cpp       ← current version source
    PluginName.o         ← current version compiled output
    History/
        PluginName_v1.cpp
        PluginName_v1.o
        PluginName_v2.cpp
        PluginName_v2.o
        ...
```

**Rules:**
- When a new plugin is started, create a folder with the plugin name in the project root.
- The plugin root folder always contains only the **most recent** `.cpp` and `.o` pair.
- When a new version is compiled, move the previous `.cpp` and `.o` into `History/` before saving the new files into the plugin root folder.
- Version files in `History/` are named with a version suffix (e.g. `_v1`, `_v2`) and are never deleted.
- The `.o` file in the plugin root is the file to copy to `/programs/plug-ins/` on the MicroSD card.

---

## What is the Disting NT?

The **Expert Sleepers disting NT** is a **Eurorack hardware module** (22HP, 25mm deep) running an embedded ARM Cortex-M7 microcontroller with Expert Sleepers proprietary firmware. It runs up to 32 signal-processing algorithms simultaneously. Built-in algorithms are called "algos". User-written compiled C++ code that runs identically is called a "plugin".

Power: 211mA (+12V) / 96mA (−12V) at idle. Does not use the +5V rail.

---

## THE MOST IMPORTANT RULE

**Plugin code does NOT run on a computer. It runs on the disting NT module itself.**

You cannot see the firmware source. You interact with it only through `distingnt/api.h` and the examples. This means:

- **Only use techniques explicitly described in the API or shown in working examples.**
- No dynamic memory allocation — no `new`/`delete`/`malloc`/`free`. Only placement `new` on firmware-provided memory blocks.
- No C++ exceptions — `-fno-exceptions` is a required compiler flag.
- No RTTI — `-fno-rtti` is a required compiler flag.
- No STL containers — no `std::vector`, `std::map`, `std::string`, etc.
- No global objects with non-trivial constructors that allocate memory.
- No threads, no OS calls, no file I/O from plugin code.
- Only use: `<math.h>` / `<cmath>`, `<cstring>`, `<cstdint>`, `<new>`.
- `step()` is a hard real-time callback. It must never block, allocate, or take unpredictable time.

Breaking these rules will crash the module, corrupt audio silently, or fail to compile.

---

## Physical Hardware

### Controls
- 3 pots (left, centre, right)
- 2 encoders (left, right — both pushable)
- 4 pushbuttons (1–4)
- OLED display (256×64 pixels, 4bpp)
- MicroSD slot (front-accessible, FAT32 or exFAT)
- USB-C (High Speed USB 2.0 device — MIDI, Audio 2.0, Video Class; does not draw power)

### Physical Inputs — 12 total
- Jacks numbered 1–12, left side of panel
- DC coupled, 3.5mm TS mono (do NOT use TRS cables)
- Two modes via rear jumper J13:
  - **Modular level:** max ±12V, impedance 101 kΩ
  - **Line level:** max ±3.5V (+10 dBu), impedance 29 kΩ

### Physical Outputs — 8 total
- 6 jacks on front panel (numbered 1–6)
- 2 additional on rear header J5 (pins 7–8)
- DC coupled, max range **±11V**

---

## The Bus System (Virtual I/O)

All signal routing uses **64 busses** internally. Bus numbers are **1-based** everywhere.

| Bus range | Purpose |
|---|---|
| 1–12 | Driven from the 12 physical inputs |
| 13–20 | Feed the 8 physical outputs (end of chain) |
| 21–64 | Aux busses for inter-algorithm routing (44 total; all fully usable by plugins) |

### Signal Flow Rules
1. Algorithms execute **top-to-bottom** in the algorithm list — strict order.
2. An algorithm only sees bus states left by algorithms **above** it.
3. At the start of `step()`: buses 1–12 already contain the current physical input samples. Output buses (13–20) and aux buses start at **zero** for the first algorithm, or carry accumulated values from algorithms above.
4. Each algorithm output has an **Output Mode**:
   - **Add (0):** `out[i] += value` — sum onto existing bus content (use for sources/instruments)
   - **Replace (1):** `out[i] = value` — overwrite bus content (use for effects/inserts)
5. **Bypass** = zero CPU, no signal passes through at all.

### Voltage Conventions in busFrames
- Float values in `busFrames` represent **volts directly**: `1.0f` = 1V on hardware.
- Audio signals: conventionally **±5.0f** (±5V peak). Never exceed ±11.0f on outputs.
- CV pitch: **1V/octave** (1.0f per octave). C4 (middle C, 261.6 Hz) at 0V is the common reference.
- Gate signals: **0.0f** = off, **5.0f** = on (5V = max velocity per polysynth convention).
- Physical inputs can deliver up to ±12.0f in modular mode.

---

## CV / MIDI / I2C Mapping (User-Facing)

- **CV mappings** are **additive offsets** on top of the preset parameter base value. `added = CV_voltage × Delta ÷ Volts`.
- **MIDI and I2C mappings directly set** parameter values (not additive).
- Multiple sources can control the same parameter simultaneously.
- CV mapping sources: (1) algorithm's own input buses, (2) physical module inputs 1–12, (3) output of another algorithm.

---

## Parameter System

- Parameters are **16-bit signed integers** (`int16_t`) with min, max, and default values.
- The firmware manages `self->v[]` — it is **read-only** from plugin code.
- `self->v[n]` is your plugin's nth parameter (0-based index matching your parameters[] array).
- `self->vIncludingCommon[]` includes firmware-managed common parameters (e.g. bypass) prepended before your own. Do not use it unless you specifically need bypass state; the exact offsets are a firmware implementation detail not exposed by the API.
- **Specifications** differ from parameters: set at instantiation, affect memory allocation, require audio interruption to change.

---

## Plugin Development

### Project Files (this repo)
- `include/distingnt/api.h` — the full plugin API
- `include/distingnt/slot.h` — for accessing other algorithms in the preset
- `Makefile` — build system for both C++ and Faust plugins
- `Disting NT Examples/LASERplugin.cpp` — a complex real-world plugin example (laser projector controller)
- `Disting NT Examples/distingNT_API/examples/` — official minimal examples: `gain.cpp`, `monosynth.cpp`, `multiple.cpp`, `fourteen.cpp`, and more

### Includes
```cpp
#include "distingnt/api.h"   // always required
#include <cmath>              // sinf, cosf, expf, logf, powf, floorf, fabsf etc.
#include <cstdint>            // uint8_t, int16_t, uint32_t etc.
#include <cstring>            // memcpy, memset
#include <new>                // placement new only
```

### Float vs Double — API Rule
The disting NT is not simply an ARM Cortex-M7 processor. It is a machine running Expert Sleepers firmware. Plugin code must comply with the firmware's API — not reason about the underlying CPU. The entire API uses `float` exclusively.

**Prefer the `f`-suffix math functions.**
- ✓ `sinf()`, `cosf()`, `expf()`, `logf()`, `powf()`, `sqrtf()`, `fabsf()`, `floorf()`, `ceilf()`
- `sin()`, `cos()`, `exp()`, `log()`, `pow()`, `sqrt()` — these silently promote arguments to `double` and return `double` under the hood, but are safe to use: Expert Sleepers use them in their own official examples (`gain.cpp` uses `log()`, `exp()`, `pow()`). Prefer the `f`-suffix versions for consistency with the API.

### Compiler Flags (Required)
```
arm-none-eabi-c++ -std=c++11 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard
  -mthumb -fno-rtti -fno-exceptions -Os -fPIC -Wall -I<path-to-include>
  -c -o YourPlugin.o YourPlugin.cpp
```

The output is a `.o` file — **not linked**. It is a relocatable object file loaded directly by the firmware. Place in `/programs/plug-ins/` on the MicroSD card.

---

## Plugin Architecture — Lifecycle

```
pluginEntry()                       ← firmware scans .o at startup
  └─ calculateStaticRequirements()  ← reserve shared memory (lookup tables)
  └─ initialise()                   ← populate shared memory — runs once
  └─ calculateRequirements()        ← called when user browses Add Algorithm
  └─ construct()                    ← called when algorithm is added to preset
       └─ [audio starts]
            └─ parameterChanged()   ← called on any parameter value change
            └─ step()               ← called every audio buffer
            └─ draw()               ← called when display needs updating
            └─ midiMessage()        ← incoming MIDI channel messages
            └─ midiRealtime()       ← incoming MIDI realtime bytes
```

**`parameterChanged()` is NOT called at construction.** Any cached derived values must be initialised to valid defaults in the constructor, not relied upon from `parameterChanged()`.

---

## The Algorithm Struct

```cpp
struct MyAlgorithm : public _NT_algorithm
{
    // Cached values from parameters (must be initialised in constructor):
    float gain;
    // Running DSP state:
    float phase;

    MyAlgorithm()
    {
        parameters       = nullptr;  // set in construct()
        parameterPages   = nullptr;  // set in construct()
        vIncludingCommon = nullptr;  // firmware-managed, read-only
        v                = nullptr;  // firmware-managed, read-only
        gain  = 1.0f;
        phase = 0.0f;
    }
};
```

For performance-critical per-sample state, use a separate DTC struct:
```cpp
struct MyAlgorithm_DTC { uint32_t phase; float filterState; };
struct MyAlgorithm : public _NT_algorithm {
    MyAlgorithm( MyAlgorithm_DTC* dtc_ ) : dtc( dtc_ ) {}
    MyAlgorithm_DTC* dtc;
};
```

---

## Defining Parameters

### _NT_parameter Struct
```cpp
struct _NT_parameter {
    const char*          name;         // display name
    int16_t              min;
    int16_t              max;
    int16_t              def;          // default value
    uint8_t              unit;         // kNT_unit...
    uint8_t              scaling;      // kNT_scaling...
    char const* const*   enumStrings;  // NULL-terminated array, or NULL
};
```

### Parameter Units
| Constant | Display |
|---|---|
| `kNT_unitNone` | raw integer |
| `kNT_unitEnum` | string from `enumStrings[]` |
| `kNT_unitDb` | dB |
| `kNT_unitDb_minInf` | dB, min shown as "−inf" |
| `kNT_unitPercent` | % |
| `kNT_unitHz` | Hz |
| `kNT_unitSemitones` | semitones |
| `kNT_unitCents` | cents |
| `kNT_unitMs` | ms |
| `kNT_unitSeconds` | s |
| `kNT_unitFrames` | frames |
| `kNT_unitMIDINote` | MIDI note |
| `kNT_unitMillivolts` | mV |
| `kNT_unitVolts` | V |
| `kNT_unitBPM` | BPM |
| `kNT_unitDegrees` | ° |
| `kNT_unitHasStrings` | calls `parameterString()` |
| `kNT_unitConfirm` | user must confirm; calls `parameterString()` |

I/O units — **only use via the macros below**:
`kNT_unitAudioInput`, `kNT_unitCvInput`, `kNT_unitAudioOutput`, `kNT_unitCvOutput`, `kNT_unitOutputMode`

### Scaling (display only — v[] always contains raw int16_t)
`kNT_scalingNone` / `kNT_scaling10` / `kNT_scaling100` / `kNT_scaling1000`

### I/O Bus Parameters — Use These Macros
```cpp
NT_PARAMETER_AUDIO_INPUT( "Input", 1, 1 )              // 1 parameter
NT_PARAMETER_CV_INPUT( "CV In", 0, 0 )                 // 1 parameter; 0 = disconnected
NT_PARAMETER_AUDIO_OUTPUT( "Output", 1, 13 )           // 1 parameter, no mode
NT_PARAMETER_CV_OUTPUT( "CV Out", 1, 13 )              // 1 parameter, no mode
NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output", 1, 13 ) // 2 parameters: bus + mode
NT_PARAMETER_CV_OUTPUT_WITH_MODE( "CV Out", 1, 13 )    // 2 parameters: bus + mode
```

**`_WITH_MODE` macros expand to TWO parameter entries.** Account for this in your enum indices. Default output bus is `13` (first physical output).

### Parameter Array and Index Enum
```cpp
enum { kParamInput, kParamOutput, kParamOutputMode, kParamGain };
//                  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^ both from _WITH_MODE macro

static const _NT_parameter parameters[] = {
    NT_PARAMETER_AUDIO_INPUT( "Input", 1, 1 )              // index 0
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output", 1, 13 ) // index 1 and 2
    { .name="Gain", .min=0, .max=100, .def=100,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL }, // index 3
};
```

For larger plugins, define parameters individually then combine with index comments:
```cpp
static const _NT_parameter allParams[] = {
    paramFreq,    //  0
    paramRotX,    //  1
    // ...
};
```

---

## Defining Parameter Pages

**Always use designated initializers with the current API.** The `_NT_parameterPage` struct gained `group` and `unused[2]` fields in a later API version. Positional initialization (as seen in the LASER plugin which targets an older API) will silently assign wrong values with the current header.

```cpp
static const uint8_t page1[] = { kParamGain };
static const uint8_t page2[] = { kParamInput, kParamOutput, kParamOutputMode };

static const _NT_parameterPage pages[] = {
    { .name="Gain",    .numParams=ARRAY_SIZE(page1), .group=0, .params=page1 },
    { .name="Routing", .numParams=ARRAY_SIZE(page2), .group=0, .params=page2 },
};

static const _NT_parameterPages parameterPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages    = pages,
};
```

`group`: if two pages share the same non-zero group number, the cursor position is preserved when switching. `group=0` defaults to the page's own index.

---

## Memory Allocation

### calculateRequirements()
```cpp
void calculateRequirements( _NT_algorithmRequirements& req, const int32_t* specifications )
{
    req.numParameters = ARRAY_SIZE(parameters);
    req.sram = sizeof(MyAlgorithm);   // instance struct
    req.dram = 0;                     // large buffers (delay lines, sample data)
    req.dtc  = sizeof(MyAlg_DTC);    // per-sample data — fastest memory
    req.itc  = 0;                     // code only (rarely needed)
}
```

### construct() — Use Placement New
```cpp
_NT_algorithm* construct( const _NT_algorithmMemoryPtrs& ptrs,
                          const _NT_algorithmRequirements& req,
                          const int32_t* specifications )
{
    MyAlgorithm* alg = new (ptrs.sram) MyAlgorithm( (MyAlg_DTC*)ptrs.dtc );
    alg->parameters     = parameters;
    alg->parameterPages = &parameterPages;
    return alg;
}
```

### Shared Memory (optional)
```cpp
static float* sharedTable = nullptr;
void calculateStaticRequirements( _NT_staticRequirements& req ) { req.dram = 1024; }
void initialise( _NT_staticMemoryPtrs& ptrs, const _NT_staticRequirements& ) {
    sharedTable = (float*)ptrs.dram;
    // populate once here
}
```

---

## parameterChanged()

Called whenever any parameter value changes. Use to cache derived values.

```cpp
void parameterChanged( _NT_algorithm* self, int p )
{
    MyAlgorithm* a = (MyAlgorithm*)self;
    switch ( p ) {
    case kParamGain:
        a->gain = a->v[kParamGain] / 100.0f;
        break;
    case kParamSmoothMs:
        a->coeff = 1.0f - expf( -1.0f / (a->v[kParamSmoothMs] * 0.001f * NT_globals.sampleRate) );
        break;
    }
}
```

- `p` is the 0-based index into your `parameters[]`.
- `self->v[p]` is the new value.
- Safe to call `NT_setParameterFromAudio()` from here.
- **NOT called at construction** — initialise cached values in the constructor.

---

## step() — Audio Processing

```cpp
void step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
    MyAlgorithm* a = (MyAlgorithm*)self;
    int numFrames  = numFramesBy4 * 4;

    // Bus N (1-based in v[]) → busFrames + (N-1) * numFrames
    const float* in  = busFrames + (a->v[kParamInput]  - 1) * numFrames;
    float*       out = busFrames + (a->v[kParamOutput] - 1) * numFrames;
    bool         rep = a->v[kParamOutputMode];  // 0=Add, 1=Replace

    for ( int i = 0; i < numFrames; ++i )
        out[i] = rep ? in[i] * a->gain : out[i] + in[i] * a->gain;
}
```

### busFrames Layout
```
busFrames + 0*numFrames  → bus 1  (physical input 1)
busFrames + 1*numFrames  → bus 2  (physical input 2)
...
busFrames + 12*numFrames → bus 13 (first physical output)
...
busFrames + 63*numFrames → bus 64 (last aux bus)
```

Bus numbers in `v[]` are **1-based**. Always subtract 1 when indexing `busFrames`.

### NT_globals
```cpp
NT_globals.sampleRate          // e.g. 48000 — always check at runtime, never hardcode
NT_globals.maxFramesPerStep    // maximum frames per step() call
NT_globals.workBuffer          // scratch buffer for use inside step() only — does not persist
NT_globals.workBufferSizeBytes
```

### Static Local Arrays in step() Are Shared
`static` local variables inside `step()` are allocated once and shared across **all instances** and **all calls**. If two instances run simultaneously they will corrupt each other. Use instance struct fields or dtc memory instead.

---

## The Factory Struct

```cpp
static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('M','y','P','l'),
    .name                        = "My Plugin",
    .description                 = "Does something",
    .numSpecifications           = 0,
    .specifications              = NULL,
    .calculateStaticRequirements = NULL,
    .initialise                  = NULL,
    .calculateRequirements       = calculateRequirements,  // required
    .construct                   = construct,              // required
    .parameterChanged            = parameterChanged,
    .step                        = step,
    .draw                        = NULL,
    .midiRealtime                = NULL,
    .midiMessage                 = NULL,
    .tags                        = kNT_tagEffect,
    .hasCustomUi                 = NULL,
    .customUi                    = NULL,
    .setupUi                     = NULL,
    .serialise                   = NULL,
    .deserialise                 = NULL,
    .midiSysEx                   = NULL,
    .parameterUiPrefix           = NULL,
    .parameterString             = NULL,
};
```

Only `construct` and `calculateRequirements` are strictly required. All other pointers can be NULL.

### GUID Rules
- `NT_MULTICHAR('A','B','C','D')` — four ASCII characters.
- **Must contain at least one capital letter.** All-lowercase is reserved for built-in algorithms.
- Must be globally unique. Check https://nt-gallery.nosuch.dev to avoid collisions.

### Tags
`kNT_tagInstrument`, `kNT_tagEffect`, `kNT_tagReverb`, `kNT_tagDelay`, `kNT_tagUtility`, `kNT_tagFilterEQ`

### Custom UI — `hasCustomUi` / `customUi` / `setupUi`

`hasCustomUi()` returns a bitmask of `_NT_controls` values indicating which controls the plugin overrides. If non-zero, `customUi()` is called with an `_NT_uiData&` describing the current control state.

```cpp
// _NT_controls bitmask — OR together what you want to override:
kNT_button1       = (1<<0)   kNT_button2      = (1<<1)
kNT_button3       = (1<<2)   kNT_button4      = (1<<3)
kNT_potButtonL    = (1<<4)   kNT_potButtonC   = (1<<5)   kNT_potButtonR   = (1<<6)
kNT_encoderButtonL= (1<<7)   kNT_encoderButtonR=(1<<8)
kNT_encoderL      = (1<<9)   kNT_encoderR     = (1<<10)
kNT_potL          = (1<<11)  kNT_potC         = (1<<12)  kNT_potR         = (1<<13)

// _NT_uiData fields passed to customUi():
struct _NT_uiData {
    float    pots[3];       // current pot positions [0.0–1.0]
    uint16_t controls;      // which controls changed this call (_NT_controls bitmask)
    uint16_t lastButtons;   // previous button states
    int8_t   encoders[2];   // encoder delta: +1, -1, or 0
    uint8_t  unused[2];
};
```

`setupUi()` is called when the algorithm's UI appears for the first time (e.g. switching from overview to algorithm display). Write current pot values [0.0–1.0] into `pots[]` to sync soft takeover.

---

## pluginEntry() — Required Entry Point

```cpp
extern "C" uintptr_t pluginEntry( _NT_selector selector, uint32_t data )
{
    switch ( selector ) {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return (uintptr_t)(data == 0 ? &factory : NULL);
    }
    return 0;
}
```

Always return `kNT_apiVersionCurrent` for new code. `extern "C"` is required for correct linkage.

---

## Specifications (Optional)

Set at instantiation; affect memory allocation; require audio interruption to change.

```cpp
static const _NT_specification specifications[] = {
    { .name="Max delay (s)", .min=1, .max=10, .def=2, .type=kNT_typeSeconds },
};
// Types: kNT_typeGeneric, kNT_typeSeconds, kNT_typeBoolean
```

---

## MIDI Callbacks

```cpp
void midiMessage( _NT_algorithm* self, uint8_t byte0, uint8_t byte1, uint8_t byte2 )
{
    int status  = byte0 & 0xF0;  // 0x80=NoteOff, 0x90=NoteOn, 0xB0=CC
    int channel = byte0 & 0x0F;  // 0–15
}
void midiRealtime( _NT_algorithm* self, uint8_t byte )
{
    // 0xF8=clock, 0xFA=start, 0xFB=continue, 0xFC=stop
}
```

### Sending MIDI
```cpp
// destination: OR of kNT_destinationBreakout, kNT_destinationSelectBus,
//              kNT_destinationUSB, kNT_destinationInternal
NT_sendMidi3ByteMessage( kNT_destinationUSB, 0xB0, ccNum, ccVal );
NT_sendMidi2ByteMessage( kNT_destinationBreakout, 0xD0, pressure );
NT_sendMidiByte( kNT_destinationSelectBus, 0xF8 );

// SysEx — call repeatedly with end=false for partial messages, end=true for final call.
// Do NOT include the 0xF7 terminator byte.
NT_sendMidiSysEx( destination, data, count, /*end=*/true );
```

### midiSysEx callback — no `self` parameter
Unlike every other callback, `midiSysEx` receives **no `_NT_algorithm*`** — it is a global handler:
```cpp
void midiSysEx( const uint8_t* message, uint32_t count ) { ... }
// In factory: .midiSysEx = midiSysEx
```

---

## draw() — Custom Display

```cpp
bool draw( _NT_algorithm* self )
{
    NT_drawText( x, y, "text", colour, kNT_textLeft, kNT_textNormal );
    NT_drawShapeI( kNT_line, x0, y0, x1, y1, colour );
    NT_drawShapeF( kNT_line, fx0, fy0, fx1, fy1, fc );
    return false;  // true = suppress standard parameter header
}
// Sizes: kNT_textTiny, kNT_textNormal, kNT_textLarge
// Align: kNT_textLeft, kNT_textCentre, kNT_textRight
// Shapes: kNT_point, kNT_line, kNT_box, kNT_rectangle, kNT_circle
// Only call draw functions from within draw(). Never from step() or elsewhere.
```

### Direct pixel buffer
```cpp
extern uint8_t NT_screen[128*64];  // 256×64 display, 4bpp — each byte = 2 pixels
```
Each byte holds two horizontally adjacent pixels (high nibble = left, low nibble = right). Can be written directly for pixel-precise rendering. Only use from within `draw()`.

### String formatting helpers
```cpp
int NT_intToString( char* buffer, int32_t value );           // returns length
int NT_floatToString( char* buffer, float value, int decimalPlaces=2 ); // returns length
```

---

## Setting Parameters Programmatically

```cpp
int32_t idx = NT_algorithmIndex( self );
NT_setParameterFromAudio( idx, kParamFoo + NT_parameterOffset(), newValue );
NT_setParameterGrayedOut( idx, kParamFoo + NT_parameterOffset(), true );
NT_setParameterFromUi( idx, kParamFoo + NT_parameterOffset(), newValue );  // UI thread only
NT_updateParameterDefinition( idx, kParamFoo );  // if parameter definition itself changed
NT_updateParameterPages( idx );

uint32_t total = NT_algorithmCount();  // total number of algorithms in preset
```

`NT_parameterOffset()` bridges 0-based plugin indices to global numbering. Always add it.

---

## Accessing Other Algorithms — slot.h

`#include <distingnt/slot.h>`

`_NT_slot` is a non-copyable handle to another algorithm in the preset. Populate it with `NT_getSlot()` (declared in `api.h`). Index is **0-based**.

```cpp
_NT_slot slot;
if ( NT_getSlot( slot, index ) ) {
    const char*     name  = slot.name();           // algorithm's display name
    uint32_t        guid  = slot.guid();            // algorithm GUID
    _NT_algorithm*  plug  = slot.plugin();          // non-NULL only if it is a plugin (not a built-in)

    uint32_t  n   = slot.numParameters();
    _NT_parameter info;
    slot.parameterInfo( info, idx );                // fills info struct; returns false if idx out of range
    int16_t   pre = slot.parameterPresetValue( idx );  // saved value, before any CV/MIDI mapping
    int16_t   cur = slot.parameterValue( idx );        // live value including all mapping
}
```

- `parameterPresetValue` — what is stored in the preset (what you set manually or via automation).
- `parameterValue` — the actual live value after any CV or MIDI mapping has been applied.

---

## Complete Minimal Plugin

```cpp
#include "distingnt/api.h"
#include <new>

struct GainAlg : public _NT_algorithm {
    float gain;
    GainAlg() : gain(1.0f) {}
};

enum { kParamIn, kParamOut, kParamOutMode, kParamGain };

static const _NT_parameter parameters[] = {
    NT_PARAMETER_AUDIO_INPUT( "Input", 1, 1 )
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output", 1, 13 )
    { .name="Gain", .min=0, .max=100, .def=100, .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
};
static const uint8_t pg1[] = { kParamGain };
static const uint8_t pg2[] = { kParamIn, kParamOut, kParamOutMode };
static const _NT_parameterPage pages[] = {
    { .name="Gain",    .numParams=1, .group=0, .params=pg1 },
    { .name="Routing", .numParams=3, .group=0, .params=pg2 },
};
static const _NT_parameterPages paramPages = { .numPages=2, .pages=pages };

void calculateRequirements( _NT_algorithmRequirements& req, const int32_t* ) {
    req.numParameters = ARRAY_SIZE(parameters);
    req.sram = sizeof(GainAlg);
    req.dram = req.dtc = req.itc = 0;
}
_NT_algorithm* construct( const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements&, const int32_t* ) {
    GainAlg* a = new (ptrs.sram) GainAlg();
    a->parameters = parameters; a->parameterPages = &paramPages;
    return a;
}
void parameterChanged( _NT_algorithm* self, int p ) {
    if ( p == kParamGain ) ((GainAlg*)self)->gain = self->v[kParamGain] / 100.0f;
}
void step( _NT_algorithm* self, float* busFrames, int numFramesBy4 ) {
    GainAlg* a = (GainAlg*)self;
    int n = numFramesBy4 * 4;
    const float* in  = busFrames + (a->v[kParamIn]  - 1) * n;
    float*       out = busFrames + (a->v[kParamOut] - 1) * n;
    bool         rep = a->v[kParamOutMode];
    for ( int i = 0; i < n; ++i )
        out[i] = rep ? in[i] * a->gain : out[i] + in[i] * a->gain;
}
static const _NT_factory factory = {
    .guid=NT_MULTICHAR('M','y','G','n'), .name="MyGain", .description="Simple gain",
    .numSpecifications=0, .specifications=NULL,
    .calculateStaticRequirements=NULL, .initialise=NULL,
    .calculateRequirements=calculateRequirements, .construct=construct,
    .parameterChanged=parameterChanged, .step=step,
    .draw=NULL, .midiRealtime=NULL, .midiMessage=NULL, .tags=kNT_tagEffect,
    .hasCustomUi=NULL, .customUi=NULL, .setupUi=NULL,
    .serialise=NULL, .deserialise=NULL, .midiSysEx=NULL,
    .parameterUiPrefix=NULL, .parameterString=NULL,
};
extern "C" uintptr_t pluginEntry( _NT_selector selector, uint32_t data ) {
    switch ( selector ) {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return (uintptr_t)(data==0 ? &factory : NULL);
    }
    return 0;
}
```

---

## Hardware I/O Reference

| Item | Value |
|---|---|
| Physical inputs | 12 (buses 1–12) |
| Physical outputs | 8 — 6 front + 2 rear (buses 13–20) |
| Aux buses | 44 (buses 21–64) |
| Total buses | 64 |
| Max algorithms per preset | 32 |
| Default sample rate | 48 kHz |
| Default buffer size | 24 frames |
| Control rate | 2 kHz |
| Modular input max | ±12V |
| Output max range | ±11V |
| Gate max velocity | 5V |
| I2C voltage scaling | 16384 = 10V |
| CPU overload threshold | 90% — module mutes all outputs |

---

## MicroSD File Structure

```
/programs/plug-ins/   → C++ plugin .o files  ← compiled plugins go here
/programs/lua/        → Lua scripts (.lua)
/ui_scripts/          → Lua UI scripts (.lua)
/presets/             → JSON preset files
/samples/<folder>/    → WAV audio samples
/wavetables/          → Wavetable WAV files
/FMSYX/               → DX7 SysEx banks (.syx, 4104 bytes)
/scl/ /kbm/ /MTS/     → Microtuning files
/MIDI/<folder>/       → MIDI song files
/recordings/          → Audio recorder output
```

---

## Development Workflow

1. Write `.cpp` in project root.
2. `make cpp SOURCE=YourPlugin.cpp OUT=YourPlugin.o`
3. Copy `.o` to `/programs/plug-ins/` on MicroSD.
4. Module scans on startup or card remount.
5. During development: use SysEx `7AH/08H` to rescan without rebooting.
6. Check `Misc / Plug-ins / View info…` on module to confirm load or see failure reason.

---

---

# Additional API Headers

---

## WAV / Sample / Wavetable / Streaming — wav.h

`#include <distingnt/wav.h>`

This header covers four related areas: sample playback from the MicroSD, wavetable loading and evaluation, and streaming audio.

### SD Card Mount Check

Always check before any card-based activity. Plugins should watch for mount/unmount in `step()`.

```cpp
bool mounted = NT_isSdCardMounted();
```

### Sample Folders and Files

Samples live in `/samples/<folder>/` on the MicroSD. Folders and files are enumerated by index.

```cpp
uint32_t nFolders = NT_getNumSampleFolders();

_NT_wavFolderInfo folderInfo;
NT_getSampleFolderInfo( folder, folderInfo );
// folderInfo.name           — folder name string
// folderInfo.numSampleFiles — number of WAV files in folder

_NT_wavInfo fileInfo;
NT_getSampleFileInfo( folder, sample, fileInfo );
// fileInfo.name       — file name string
// fileInfo.numFrames  — total sample frames
// fileInfo.sampleRate — e.g. 44100, 48000
// fileInfo.channels   — kNT_WavMono or kNT_WavStereo
// fileInfo.bits       — kNT_WavBits8 / kNT_WavBits16 / kNT_WavBits24 / kNT_WavBits32 (float)
```

### Reading Sample Frames (async, callback-based)

Loading happens asynchronously. The `_NT_wavRequest` struct **must persist** until the callback fires — do not declare it on the stack.

```cpp
_NT_wavRequest req;
req.folder      = 0;
req.sample      = 0;
req.dst         = myBuffer;           // pre-allocated memory in dram
req.numFrames   = totalFrames;
req.startOffset = 0;
req.channels    = kNT_WavMono;
req.bits        = kNT_WavBits16;
req.progress    = kNT_WavProgress;    // show progress indicator; kNT_WavNoProgress to hide
req.callback    = myCallback;         // void callback(void* data, bool success)
req.callbackData = this;
bool ok = NT_readSampleFrames( req ); // returns true if read was initiated
```

Format conversion is automatic: if `channels`/`bits` don't match the file, the firmware converts on load.

**Channel enum:** `kNT_WavMono`, `kNT_WavStereo`
**Bit depth enum:** `kNT_WavBits8`, `kNT_WavBits16`, `kNT_WavBits24`, `kNT_WavBits32` (float)

### Wavetables

Wavetables live in `/wavetables/` on the MicroSD. Each wavetable contains multiple single-cycle waves of the same length.

```cpp
uint32_t nWT = NT_getNumWavetables();

_NT_wavetableInfo wtInfo;
NT_getWavetableInfo( index, wtInfo );
// wtInfo.name — wavetable name string

_NT_wavetableRequest wtReq;
wtReq.index         = index;
wtReq.table         = myInt16Buffer;  // pre-allocated dram
wtReq.tableSize     = bufferSizeInFrames;
wtReq.callback      = myCallback;    // void callback(void*)
wtReq.callbackData  = this;
bool ok = NT_readWavetable( wtReq ); // returns true if read was initiated
// After callback:
// wtReq.error       — true if load failed
// wtReq.usingMipMaps — true if the table has mipmap levels
// wtReq.numWaves    — number of waves in the table
// wtReq.waveLength  — frames per wave
```

**Wavetable memory layout:**
- **No mipmaps:** waves contiguous — `wave[n]` starts at `n * waveLength`.
- **With mipmaps:** power-of-two downsampled versions prepended. Mipmap of size `s` is at offset `s * numWaves`. Full-size table is at `waveLength * numWaves`.

**Evaluating a wavetable** (convenience function, returns value in ±1.0):

```cpp
_NT_wavetableEvaluation eval;
eval.phase     = phase;      // oscillator phase 0.0 <= phase < 1.0
eval.offset    = position;   // position within wavetable 0 <= offset < (numWaves-1)
eval.frequency = freq;       // oscillator frequency in Hz
// eval.mipmapSize — (output) which mipmap level was chosen

float sample = NT_evaluateWavetable( wtReq, eval );
```

### Streaming Audio (from SD card in real time)

For playing long samples without loading them fully into memory. The stream object is an opaque pointer; allocate its backing memory using `NT_globals.streamSizeBytes`.

```cpp
// Allocate stream storage in dram (size = NT_globals.streamSizeBytes)
_NT_stream stream = (void*)myStreamMemory;

_NT_streamOpenData data;
data.streamBuffer  = myStreamBuffer;   // size = NT_globals.streamBufferSizeBytes
data.folder        = folder;
data.sample        = sample;
data.velocity      = 1.0f;             // 0.0–1.0 for velocity switching
data.startOffset   = 0;               // start frame
data.reverse       = false;
data.rrMode        = kNT_RRModeSequential;  // round-robin mode

bool ok = NT_streamOpen( stream, data );

// In step(), render into an _NT_frame buffer:
uint32_t rendered = NT_streamRender( stream, frameBuffer, numFrames, speed );
// speed = sample frames per render frame (1.0 = normal, 2.0 = double speed, etc.)
// Returns number of frames actually rendered (may be less at end of sample).
```

**Round-robin modes:** `kNT_RRModeSequential`, `kNT_RRModeRandom`, `kNT_RRModeRandom2`, `kNT_RRModeRandom3`

---

## Serialisation — serialisation.h

`#include <distingnt/serialisation.h>`

Used in the `serialise` and `deserialise` factory callbacks to save and restore plugin state to/from the preset JSON. These callbacks are optional — if NULL in the factory, only the standard parameter values are saved.

### Serialising (writing state)

The factory's `serialise` callback receives an `_NT_jsonStream&`. Write your state as a JSON structure:

```cpp
void serialise( _NT_algorithm* self, _NT_jsonStream& s )
{
    MyAlg* a = (MyAlg*)self;
    s.openObject();
        s.addMemberName( "version" );  s.addNumber( 1 );
        s.addMemberName( "data" );
        s.openArray();
            for ( int i = 0; i < N; ++i )
                s.addNumber( (int)a->myData[i] );
        s.closeArray();
    s.closeObject();
}
```

**`_NT_jsonStream` methods:**

| Method | Purpose |
|---|---|
| `openArray()` / `closeArray()` | Array container |
| `openObject()` / `closeObject()` | Object container |
| `addMemberName(str)` | Key for next member in object |
| `addNumber(int)` / `addNumber(float)` | Integer or float value |
| `addString(str)` | String value |
| `addFourCC(uint32_t)` | Four-char code as string |
| `addBoolean(bool)` | Boolean value |
| `addNull()` | Null value |

### Deserialising (reading state)

The factory's `deserialise` callback receives an `_NT_jsonParse&`. Parse back what was written:

```cpp
bool deserialise( _NT_algorithm* self, _NT_jsonParse& p )  // returns bool success
{
    MyAlg* a = (MyAlg*)self;
    int num;
    if ( !p.numberOfObjectMembers(num) ) return false;
    for ( int m = 0; m < num; ++m ) {
        if ( p.matchName("data") ) {
            int n;
            if ( p.numberOfArrayElements(n) ) {
                for ( int i = 0; i < n && i < N; ++i )
                    p.number( a->myData[i] );
            }
        } else {
            p.skipMember();
        }
    }
    return true;
}
```

**`_NT_jsonParse` methods:**

| Method | Purpose |
|---|---|
| `numberOfArrayElements(int& n)` | True + count if current token is array |
| `numberOfObjectMembers(int& n)` | True + count if current token is object |
| `matchName(str)` | True if current token matches key name |
| `skipMember()` | Skip a member (name + value) |
| `number(int& v)` / `number(float& v)` | Parse integer or float |
| `string(const char*& str)` | Parse string (pointer into firmware buffer — copy if needed) |
| `boolean(bool& v)` | Parse boolean |
| `null()` | Consume a null |

All parse methods return `false` on failure (type mismatch, end of data). Always check.

---

## Microtuning — microtuning.h

`#include <distingnt/microtuning.h>`

Provides access to `.scl` (Scala scale) files stored in `/scl/` on the MicroSD. Used to implement alternative tuning systems.

### Listing SCL Files

```cpp
uint32_t n = NT_getNumScl();

_NT_sclInfo info;
NT_getSclInfo( index, info );
// info.name — scale name string
```

### Reading an SCL File (async, callback-based)

Like sample loading, this is asynchronous. The `_NT_sclRequest` struct must persist until the callback fires.

```cpp
_NT_sclNote notes[128];    // pre-allocated memory
char nameBuf[64];
char descBuf[256];

_NT_sclRequest req;
req.index              = index;
req.notes              = notes;
req.maxNotes           = 128;
req.nameBuffer         = nameBuf;         // can be NULL
req.nameBufferSize     = sizeof(nameBuf);
req.descriptionBuffer  = descBuf;         // can be NULL
req.descriptionBufferSize = sizeof(descBuf);
req.callback           = myCallback;      // void callback(void*)
req.callbackData       = this;

bool ok = NT_readScl( req );   // returns true if read was initiated
// After callback:
// req.error    — true if load failed
// req.numNotes — number of notes in the scale
// req.notes[]  — populated note data
```

### `_NT_sclNote` Union

Each note in a scale is stored as a union of two representations:

```cpp
union _NT_sclNote {
    double octaves;                  // cents/1200 — if specified as a floating-point number in the file
    struct {
        int32_t numeratorValue;
        int32_t denominatorValue;    // stored negative to distinguish from float form
    };
};

bool     isRatio  = note.isRatio();       // true if the note was specified as a ratio
uint32_t num      = note.numerator();     // ratio numerator
uint32_t den      = note.denominator();   // ratio denominator (always positive)
double   oct      = note.octaves;         // cents/1200 value if not a ratio
```

To convert to frequency ratio: if `isRatio()`, ratio = `numerator / denominator`. If not, ratio = `pow(2.0, octaves)`.

---

# Lua Scripting

The disting NT embeds **Lua 5.4.6**. All scripts share a single global Lua instance on the module. There are three access points:

1. **Lua Script algorithm** — scripted plugins that run as part of the audio/CV bus processing chain.
2. **UI scripts** — take over the hardware UI (pots, encoders, buttons, display).
3. **Lua console tool** — browser-based REPL communicating via MIDI SysEx (lives in the Expert Sleepers GitHub repo). Useful for iteration without touching the MicroSD card.

Scripts go in `/programs/lua/` (Lua Script algorithm) or `/ui_scripts/` (UI scripts) on the MicroSD card.

---

## Lua Script Algorithm

A script file returns a table of named functions. The first two comment lines are the script's name and description, shown in the module UI before loading.

### init(self)

Called once on load. Must return a table declaring inputs, outputs, and optionally parameters.

```lua
return {
    inputs  = 2,       -- integer, or array of kCV/kGate/kTrigger
    outputs = 2,       -- integer, or array of kStepped/kLinear
    parameters = { ... }
}
```

- `inputs` and `outputs` range from 0 to 28 (the number of busses).
- The algorithm automatically exposes routing parameters for each input and output.
- Omitting either defaults to 0.

**Input types** (use array form to set type per input):
- `kCV` — raw CV voltage
- `kGate` — calls `gate(self, input, rising)` on change instead of polling in `step`
- `kTrigger` — calls `trigger(self, input)` on trigger instead of polling in `step`

**Output types** (use array form to set type per output):
- `kStepped` — updates once per `step` call (default)
- `kLinear` — linearly interpolated between steps (better for smooth CVs)

### step(self, dt, inputs)

Called every **1ms**. `dt` = elapsed seconds since last call. `inputs[]` = 1-based array of bus voltages for the declared inputs. Returns a table of output voltages (sparse OK — omitted outputs retain previous value).

```lua
step = function(self, dt, inputs)
    local f = 1 + inputs[1]
    -- compute outputs...
    return { out1, out2 }
end
```

### trigger(self, input) / gate(self, input, rising)

Efficient callbacks used instead of polling in `step` when inputs are typed `kTrigger` or `kGate`. `input` = which input fired (1-based). `rising` (gate only) = `true` if gate opened. Return a sparse table of output voltages to update.

### draw(self)

Called at **30fps** for custom display. All `drawXXX` calls must be made from within `draw`. Return `true` to suppress the standard parameter header line.

### Custom UI (ui function)

If the script defines a `ui` function that returns `true`, the standard pot/encoder UI is suppressed. The script then handles events via the same callbacks as UI scripts (see below).

---

## Parameters in Lua Scripts

Declared in the `init` return table. Three forms:

```lua
parameters = {
    { "Name", min, max, default, kUnit },              -- integer parameter
    { "Name", min, max, default, kUnit, kBy10 },       -- scaled (float displayed)
    { "Name", { "Option1", "Option2" }, default },     -- enum
}
```

**Unit constants:** `kNone`, `kDb`, `kDb_minInf`, `kPercent`, `kHz`, `kSemitones`, `kCents`, `kMs`, `kSeconds`, `kFrames`, `kMIDINote`, `kMillivolts`, `kVolts`, `kBPM`

**Scale constants:** `kBy10`, `kBy100`, `kBy1000` — divides min/max/default for float display.

Values are read-only via `self.parameters[n]` (1-based, script-relative). To change a value use `setParameter`. `self.parameterOffset` converts script-relative indices to the global parameter indices required by the global functions.

---

## UI Scripts

A UI script takes over the hardware UI entirely. Same table-of-functions structure. `init()` returns `true` on success or an error string on failure.

**Event callbacks the script can define:**

| Callback | Argument | Notes |
|---|---|---|
| `pot1Turn / pot2Turn / pot3Turn` | `value` ∈ [0.0, 1.0] | Pot position |
| `encoder1Turn / encoder2Turn` | `+1` or `-1` | Clockwise / anticlockwise |
| `button1Push`…`button4Push` | none | |
| `button1Release`…`button4Release` | none | |
| `pot1Push / pot2Push / pot3Push` | none | Pots are pushable |
| `pot1Release / pot2Release / pot3Release` | none | |
| `encoder1Push / encoder2Push` | none | |
| `encoder1Release / encoder2Release` | none | |

`standardPot1Turn(value)` / `standardPot2Turn` / `standardPot3Turn` — delegates to the default pot behaviour (page select, parameter select, value change). Use when you want to pass control back to the standard UI for specific pots.

`exit()` — returns control from a UI script to the normal module UI.

---

## Global Functions (all Lua contexts)

### Algorithm & parameter lookup

```lua
alg   = findAlgorithm( "Algorithm Name" )    -- returns 1-based index (or nil)
param = findParameter( alg, "Param Name" )   -- returns 1-based index (or nil)
n     = getAlgorithmCount()                  -- total number of algorithms in preset  [v1.10]
name  = getAlgorithmName( alg )              -- algorithm display name                [v1.10]
n     = getParameterCount( alg )             -- number of parameters for algorithm    [v1.10]
name  = getParameterName( alg, index )       -- parameter name string                 [v1.10]
```

`findParameter` with a prefixed name like `"1:Speed"` selects a specific channel of a multi-channel algorithm. Without a prefix, all matching channels are returned.

### Parameter access

```lua
v = getParameter( alg, param )                         -- read value (integer)
setParameter( alg, param, value )                      -- set by integer value
setParameter( alg, param, value, true )                -- set and focus that parameter [v1.8]
setParameterNormalized( alg, param, 0.0..1.0 )         -- set by normalized range
setParameterNormalized( alg, param, 0.0..1.0, true )   -- set and focus               [v1.8]
```

### Navigation

```lua
focusParameter( alg, param )       -- set what drawStandardParameterLine shows
alg   = getCurrentAlgorithm()      -- currently highlighted algorithm (1-based)
param = getCurrentParameter( alg ) -- current parameter for that algorithm
```

### Bus voltage

```lua
v = getBusVoltage( alg, bus )   -- bus is 0-based; alg ranges 0..numAlgorithms
```

Returns the voltage on a bus at the boundary of the specified algorithm.

### MIDI output [v1.9]

```lua
sendMIDI( where, byte1 )               -- 1-byte message
sendMIDI( where, byte1, byte2 )        -- 2-byte message
sendMIDI( where, byte1, byte2, byte3 ) -- 3-byte message
```

`where` bitmask destinations (OR together as needed):
- `0x1` = MIDI breakout
- `0x2` = Select Bus
- `0x4` = USB
- `0x8` = Internal

### I2C [v1.11]

```lua
sendI2CCommand( address, byte1, byte2, ... )              -- send I2C command
bytes = sendI2CGetter( address, responseBytes, byte1, ... ) -- send and read response
```

### Display control [v1.10]

```lua
setDisplayMode( "overview" )     -- algorithm overview screen
setDisplayMode( "meters" )       -- VU meters
setDisplayMode( "parameters" )   -- parameter list
setDisplayMode( "ui" )           -- algorithm custom UI
setDisplayMode( "algorithm" )    -- algorithm view (standard parameter display)
setDisplayMode( "menu" )         -- menu
```

### Utilities

```lua
count = getCpuCycleCount()                     -- 32-bit CPU cycle counter for profiling [v1.10]
a, b, c, str = getFirmwareVersion()            -- major, minor, point, full string       [v1.13]
```

---

## Preset Serialisation from Lua Scripts [v1.7]

Scripts can persist arbitrary data into the preset JSON by defining a `serialise` function. On preset save the firmware calls it; on preset load the saved data is available as `self.state` **before** `init` is called.

```lua
return {
    name = 'MyScript',

    serialise = function( self )
        return {
            version   = 1,
            snapshots = self.snapshots,   -- any JSON-friendly table
        }
    end,

    init = function( self )
        -- self.state contains whatever serialise returned last save (or nil on first load)
        if self.state then
            self.snapshots = self.state.snapshots or {}
        else
            self.snapshots = {}
        end
        return { inputs = 0, outputs = 0 }
    end,
}
```

Rules:
- Data must be JSON-friendly: numbers, strings, booleans, and tables/arrays of those. No functions, no userdata.
- `self.state` is `nil` if the preset has never been saved with this script's data.
- Standard parameter values are always saved automatically — `serialise` is only needed for extra state.

---

## MIDI Input in Lua Scripts [v1.9]

Declare MIDI input in `init` and define a `midiMessage` callback:

```lua
init = function( self )
    return {
        inputs  = 0,
        outputs = 0,
        midi = {
            channelParameter = 1,         -- index of script parameter that holds the MIDI channel
            messages = { 0xB0, 0x90 },    -- message types to receive (CC, NoteOn, etc.)
        },
    }
end,

midiMessage = function( self, message )
    -- message is a table: message[1]=status byte, message[2]=byte2, message[3]=byte3
end,
```

---

## Library Files [v1.9]

Scripts can use `require` to load Lua library files from `/programs/lua/lib/` on the MicroSD:

```lua
local mylib = require( 'mylib' )   -- loads /programs/lua/lib/mylib.lua
```

---

## self.algorithmIndex [v1.10]

The firmware automatically sets `self.algorithmIndex` to the 1-based index of the script's own algorithm in the preset. Available during all callbacks including `init`.

---

## Drawing Functions (call only from draw())

Display is **256×64 pixels**, origin top-left. Colour range **0** (off) to **15** (full brightness).

| Function | Description |
|---|---|
| `drawText(x, y, str)` | Draw string; y = baseline |
| `drawTinyText(x, y, str, col)` | 3×5 pixel font [v1.8] |
| `drawLine(x0, y0, x1, y1, col)` | Integer-coord line |
| `drawSmoothLine(x0, y0, x1, y1, col)` | Antialiased line, float coords |
| `drawBox(x0, y0, x1, y1, col)` | Unfilled rectangle |
| `drawRectangle(x0, y0, x1, y1, col)` | Filled rectangle |
| `drawCircle(x, y, r, col)` | Circle [v1.8] |
| `drawSmoothCircle(x, y, r, col)` | Antialiased circle [v1.8] |
| `drawStandardParameterLine()` | Standard top-bar (most recently changed parameter) |
| `drawParameterLine(alg, param, yOffset)` | Parameter line for specific alg/param |
| `drawAlgorithmUI(alg)` | Draw an algorithm's custom GUI |

---

## Lua API Version History Summary

| Firmware | Lua additions |
|---|---|
| 1.6.0 | Lua Script algorithm introduced; console tool; base API |
| 1.7.0 | `serialise` / `self.state` preset persistence; custom UI (`ui` function) |
| 1.8.0 | `drawCircle`, `drawSmoothCircle`, `drawTinyText`; `setParameter` focus arg; `getCurrentAlgorithm`, `getCurrentParameter` |
| 1.9.0 | `require` for lib files; `sendMIDI`; `midiMessage` callback |
| 1.10.0 | `getAlgorithmCount`, `getAlgorithmName`, `getParameterCount`, `getParameterName`, `getCpuCycleCount`, `setDisplayMode`; `self.algorithmIndex` |
| 1.11.0 | `sendI2CCommand`, `sendI2CGetter` |
| 1.13.0 | `getFirmwareVersion()` |

---

## Lua Console Tool

Browser JS app in the Expert Sleepers GitHub repo. Communicates via MIDI SysEx.

- **Interactive REPL**: commands execute immediately on the module; results returned. Useful for inspecting global state, checking Lua syntax.
- **Install Program**: paste a Lua Script into the box, click Install (or Ctrl+Enter) to load it into the running Lua Script algorithm in memory — no MicroSD write required. Effect is equivalent to deleting and reloading the algorithm. No state is preserved.
- Keyboard shortcuts: Enter = execute, Ctrl+L = clear, ↑ = recall previous, Shift+Enter = newline without execute.

---

# Modular Synthesizer Knowledge

This section provides foundational knowledge about modular synthesis concepts to inform plugin design decisions. It applies to how signals are structured and how synthesis voices are built — relevant when designing the DSP behaviour of a plugin.

---

## Signal Types in Eurorack

All signals are analog voltages. The same jack and cable type is used for everything — the distinction is conceptual.

| Signal Type | Typical Range | Notes |
|---|---|---|
| Audio | ±5V (10Vpp) | May clip at ±12V rail |
| Unipolar CV | 0–10V | Envelopes, modulation depths |
| Bipolar CV | ±5V | LFOs, attenuverter outputs |
| Pitch CV (1V/oct) | 0–8V typical | C4 at 0V; 1 semitone ≈ 83.3 mV |
| Gate / Trigger | 0V / +5V | Anything >~1V registers as high |

- **Gate**: stays high for the duration of a note. Drives envelope hold (sustain phase).
- **Trigger**: a brief pulse (~ms). Fires one-shot events, retriggering, clock steps.
- **Clock**: regular trigger stream at a tempo. Divided/multiplied to derive rhythmic subdivisions.

### Pitch CV (1V/oct)
- Each 1V increase raises pitch by one octave.
- 12 semitones distributed linearly within 1V — 1 semitone = ~83.3 mV.
- C4 (middle C, 261.6 Hz) at 0V is the standard reference (VCV Rack, most modern Eurorack).
- In busFrames: `freq = 261.6f * powf(2.0f, cv)` where `cv` is the float bus value in volts.

---

## The Basic Synthesis Voice

### East Coast (Moog-style subtractive)
```
[VCO] → [VCF] → [VCA] → output
          ↑        ↑
       [EG2]    [EG1]     ← triggered by Gate
[Sequencer] → Pitch CV → [VCO]
```
- **VCO**: continuous oscillator. Never stops — the VCA creates silence between notes.
- **VCF**: shapes timbre by filtering harmonics.
- **VCA**: controls amplitude. Without it the oscillator drones forever.
- **EG**: shapes how VCF and VCA change over time in response to a gate.

### West Coast (Buchla-style)
Complex oscillators (FM, waveshaping) fed through a Low Pass Gate (LPG) combining VCA and filter behaviour. Punchier, more percussive, less reliance on filtering.

---

## VCO — Voltage Controlled Oscillator

### Waveforms
- **Sine**: no harmonics beyond fundamental. Pure, soft.
- **Triangle**: odd harmonics, attenuates quickly. Slightly brighter than sine.
- **Sawtooth**: all harmonics (odd and even). Bright, buzzy — classic subtractive source.
- **Square/Pulse**: odd harmonics only. Hollow, woodwind-like. Pulse width affects timbre (PWM).
- **Noise**: all frequencies simultaneously. Percussion, wind, random modulation.

### VCO Inputs
- **1V/oct CV**: sets pitch
- **FM (linear or exponential)**: audio-rate = sidebands / complex timbres; LFO-rate = vibrato
- **Sync**: hard sync resets the waveform to phase of an incoming signal

### LFO
Same circuit as VCO but running below audio rate (~0.01–20 Hz). Used as modulation source:
- LFO → VCO pitch = vibrato
- LFO → VCF cutoff = filter wobble
- LFO → VCA = tremolo
- LFO → PWM = pulse width animation

---

## VCF — Voltage Controlled Filter

### Filter Types
- **Low-pass (LPF)**: passes below cutoff, attenuates above. Most common.
- **High-pass (HPF)**: passes above cutoff. Thins the sound.
- **Band-pass (BPF)**: passes a band around cutoff. Nasal, focused.
- **Notch**: attenuates a narrow band, passes everything else.
- **State variable / multimode**: multiple outputs simultaneously.

### Key Parameters
- **Cutoff frequency**: CV-controllable. Sweep = the classic subtractive sound.
- **Resonance (Q)**: emphasizes frequencies near cutoff. At high resonance: self-oscillation (sine wave with no audio input).
- **Filter slope**: 12 dB/oct (2-pole) or 24 dB/oct (4-pole). Steeper = more dramatic.
- **Key tracking**: portion of pitch CV routed to cutoff to keep filter musically related to note. 100% = filter moves exactly one octave per octave of pitch.

The filter envelope (EG2 in the classic voice) sweeps cutoff over time — fast attack, medium decay, zero sustain = classic percussive pluck shape.

---

## VCA — Voltage Controlled Amplifier

- Signal in × CV level = output amplitude.
- 0V CV = silence. Max CV = full signal.
- VCAs are not only for audio — they scale any CV signal.
- **Linear VCA**: equal CV steps = equal amplitude steps. Technically correct but sounds non-uniform perceptually.
- **Exponential VCA**: equal CV steps = equal perceived loudness steps. More natural for musical use.
- You can never have enough VCAs. Uses include: modulation depth control, AM synthesis (audio-rate CV input), velocity scaling, dynamic effect routing.

---

## Envelope Generators

An EG produces a shaped CV trajectory in response to a gate or trigger. It defines how a parameter changes over time during a note.

### ADSR

```
         /\
        /  \_________ (sustain level)
       /               \
------                   ------
  A     D      S           R
```

1. **Attack**: gate-on → peak. Short = percussive. Long = slow swell.
2. **Decay**: peak → sustain level.
3. **Sustain**: level held while gate is high. This is a **level**, not a time.
4. **Release**: gate-off → zero.

### AD (Attack-Decay)
No sustain, no release. Fires fully from a trigger and returns to zero. Common for percussion and modulation.

### AHD (Attack-Hold-Decay)
A one-shot envelope primarily used for **amplitude shaping** of percussive sounds.

```
         /‾‾‾‾‾\
        /        \
       /           \
------               ------
   A       H       D
```

1. **Attack**: rises to peak.
2. **Hold**: stays at peak for a timed flat period before falling. This is the defining feature.
3. **Decay**: falls from peak to zero.

AHD is **gate-independent** — it runs its full shape regardless of gate length. This makes it predictable and useful for drum synthesis where you want a consistent hit shape.

- **Zero hold = same as AD.**
- Primarily used for **volume envelopes** on percussive sounds.
- The hold segment adds a "dwell" at the top — useful for sustaining the full amplitude of a hit before the tail begins.

### Pitch Envelopes in Drum Synthesis
Pitch envelopes are typically **simple decay envelopes** — most often just a single exponential decay from a high starting frequency down to a resting pitch. No hold or attack phase is needed; the sweep starts immediately at the trigger and decays to the base pitch. A plain AD envelope (or even just a one-pole exponential decay implemented directly in DSP) is the standard approach.

### Envelope Curves
- **Exponential** (most common in analog): rate of change slows near the target. Natural-sounding. Mirrors RC circuits and human loudness perception.
- **Linear**: constant rate. More mechanical.
- Exponential envelope + linear VCA ≈ linear envelope + exponential VCA in perceived result — the combination matters.
- Ultra-fast attacks often *sound* slower than moderately fast attacks due to auditory illusions. A slightly non-zero attack can sound crisper than zero attack.

---

## Drum Synthesis

Electronic drum sounds combine oscillators, noise, envelopes, filters, and VCAs in specific ways.

### Kick Drum
1. **Sine or triangle oscillator** — the body. Low-frequency fundamental is the sound.
2. **Pitch envelope (simple exponential decay)**: sweeps oscillator frequency from a high starting pitch (~150–400 Hz) down to the resting pitch (~40–80 Hz). Exponential decay is critical — linear sweeps sound unnatural. The speed and curve define the character.
3. **Amplitude envelope (AHD or AD)**: fast attack, exponential decay (200–800 ms).
4. **Optional VCF**: low-pass to tune the body thickness.

Key insight: the pitch sweep defines the character more than the waveform. Pitch start, pitch end, and decay curve are the primary tonal controls.

### 808-Style Bass
When the kick's pitch decay is slowed and the resting pitch is raised into a musical range, the kick becomes a sequenceable pitched bass tone. Controlled via 1V/oct CV. VCA decay determines note length.

### Snare
1. **Tone body**: sine/triangle at ~180–250 Hz, short AHD amplitude envelope — the "thwack".
2. **Noise burst**: white noise through VCA with short-to-medium AHD amplitude envelope, shaped by a HPF or BPF — the "snare rattle".
3. Mix ratio of tone to noise determines acoustic vs. electronic character.

### Hi-Hat
1. **White noise** (or 6 detuned square oscillators, TR-808 technique).
2. **High-pass or band-pass filter**: removes low end.
3. **VCA amplitude envelope**:
   - Closed: very short decay (~a few ms to 100 ms).
   - Open: longer decay (~300 ms–1 s). Closed hat triggered to cut off open hat's tail.

### Clap
Noise burst through VCA with band-pass filter (~1–3 kHz). Multiple rapid noise bursts in succession simulate separate hand slaps, followed by a longer noise tail.

### General Drum Synthesis Principles
- **Pitch envelopes define character**: the pitch trajectory matters more than the waveform. A sine with the right pitch sweep is a convincing drum.
- **Exponential decays sound natural**: both amplitude and pitch decays should be exponential.
- **Short = tight, long = loose**: amplitude decay time is the primary control over the feel of a hit.
- **Layering**: most convincing sounds use at least two layers (tone + noise for snare, click + body for kick).
- **Waveshaping/saturation on kick body**: adds harmonics and presence, helps the kick cut through a mix.
- **Noise character**: white noise = equal energy per frequency. Pink noise = more low-end warmth. Filter on noise source dramatically changes character.

---

## Utilities

- **Attenuator**: scales signal down (0–1).
- **Attenuverter**: scales −1 to +1. Allows polarity inversion.
- **Offset/Voltage Source**: adds fixed DC. Used to transpose, bias, shift.
- **Mixer**: sums signals. CV domain: combine LFOs for complex modulation. Audio: richer timbres.
- **Multiple / Buffered Multiple**: splits one signal. Buffered = op-amp driven, preserves pitch CV accuracy.
- **Sample & Hold**: samples input on trigger, holds until next trigger. Noise + S&H + clock = random melody.
- **Slew Limiter**: smooths sudden transitions. Portamento/glide. Also a basic function generator.
- **Logic (AND, OR, XOR)**: combines gate signals for complex rhythmic interactions.
- **Clock Divider/Multiplier**: derives slower/faster clocks from master. Essential for polyrhythm.
- **Quantizer**: rounds CV to nearest note of a scale. Turns generative CV into musical pitches.

---

## Patching Philosophy

- Everything is a voltage. Any output can technically go to any input.
- Never connect two outputs together without a mixer — may damage modules.
- **Normalization**: some jacks have a default internal connection overridden when a cable is plugged in.
- Self-patching (output back to own input) is a primary creative technique (self-FM, envelope feedback, etc.).

---

---

# OPEN ISSUES — TO BE SORTED OUT

The following gaps were identified and need to be verified against the hardware, firmware, or API before being folded into the main documentation as authoritative facts.

**2. ~~Float vs. double — RESOLVED.~~**
Expert Sleepers use un-suffixed `log()`, `exp()`, `pow()` in `gain.cpp` and `sin()` extensively in airwindows examples. These are safe on the disting NT. The API rule is updated: prefer `f`-suffix versions for consistency, but the un-suffixed versions compile and run correctly.

**3. ~~Exact voltage reference for CV pitch — REMOVED.~~**
Plugins will have a frequency offset parameter, making the absolute CV reference irrelevant in practice.

**4. ~~_NT_parameterPage struct — PARTIALLY RESOLVED.~~**
`group` and `unused[2]` were added in GitHub commit `10c11c7`. Designated initializers with `.group=0` are safe on all firmware versions: the firmware explicitly sets `group` to the page index when it is 0, guaranteeing backward compatibility. The exact API version number where this commit landed is unknown, but the rule stands: always use designated initializers and always set `.group=0` unless intentionally grouping pages.

**5. ~~parameterChanged() at construction — RESOLVED.~~**
Confirmed: `v[]` contains the default values from `_NT_parameter.def` when `construct()` returns. `parameterChanged()` is NOT called at construction. The plugin starts in its defined default state immediately when loaded. Consequence: `v[]` can be read safely in the constructor. However, any derived/computed cached values (e.g. `gain = v[kParamGain] / 100.0f`) must still be initialized in the constructor, because `parameterChanged()` will not be called until the user changes a value after loading.

**6. ~~Bus state at the start of step() — RESOLVED.~~**
Confirmed: at the start of each audio block, before any algorithm runs, output and aux buses (13–64) are zeroed. Buses 1–12 contain the current physical input samples. From there, each algorithm builds up the bus state according to the signal flow — using add or replace output mode.

**7. ~~extern "C" — RESOLVED.~~**
Always write `extern "C"` explicitly on the `pluginEntry` definition. This prevents C++ name mangling so the firmware can find the function by name in the `.o` file. Although omitting it may work (because `api.h` declares it inside an `extern "C"` block), always include it explicitly for clarity and safety.

**8. ~~API version targeting — RESOLVED.~~**
Always use `kNT_apiVersionCurrent` (v13). The current firmware is 1.15.0 which corresponds to API v13 per Expert Sleepers. Always target v13 for new code.

**9. ~~vIncludingCommon — RESOLVED.~~**
Bypass is the only common parameter. Do not use `vIncludingCommon` — always use `v[]` with 0-based indexing into your own plugin parameters.

**10. Existing project plugin files — audit and document.**
`2kVCO/2kVCO.cpp` exists in the project. Read it and record what it does, what patterns it uses, and whether it is a stable reference. Keep a brief inventory of all plugin `.cpp` files in this project and their status.
