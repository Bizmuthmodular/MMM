# Disting NT — C++ Plugin Programming Reference

---

## THE MOST IMPORTANT RULE

**The code you write does NOT run on a computer. It runs on the disting NT module itself — an embedded ARM Cortex-M7 microcontroller running Expert Sleepers firmware.**

You cannot see the firmware source. You can only interact with it through the API described in `distingnt/api.h` and demonstrated in the official examples. This means:

- **Only use programming techniques that are explicitly described in the API or shown in working examples.** If it is not in the API or examples, do not assume it works.
- No dynamic memory allocation (`new`/`delete`, `malloc`/`free`). The only exception is placement `new` on memory blocks provided by the firmware.
- No C++ exceptions (`-fno-exceptions` is a required compiler flag).
- No RTTI (`-fno-rtti` is a required compiler flag).
- No standard library containers (`std::vector`, `std::map`, `std::string`, etc.) — they require heap allocation which is not available.
- No global objects with non-trivial constructors that allocate memory.
- No threads, no OS calls, no file I/O from within your plugin code.
- Only use C standard library headers that provide math/memory functions: `<math.h>` or `<cmath>`, `<cstring>`, `<cstdint>`, `<new>` (for placement new only).
- The module runs at audio-rate in a real-time context. The `step()` function is called thousands of times per second. It must never block, never allocate, and must complete as fast as possible.

Breaking any of these rules will either crash the module, corrupt audio silently, or simply not compile.

---

## Includes

Every plugin starts with exactly these includes (use only what you need):

```cpp
#include "distingnt/api.h"   // The entire plugin API — always required
#include <cmath>              // sinf, cosf, expf, logf, powf, floorf, fabsf etc.
#include <cstdint>            // uint8_t, int16_t, uint32_t etc.
#include <cstring>            // memcpy, memset
#include <new>                // placement new operator
```

You may also use `<math.h>` instead of `<cmath>` — both work. The API header itself only pulls in `<stdint.h>`.

**Never include:** `<vector>`, `<string>`, `<map>`, `<iostream>`, `<stdlib.h>` for `malloc`, or any other header that implies heap allocation.

---

## Compiler Flags (Required)

The plugin must be compiled to a `.o` (object file) for the ARM Cortex-M7 with these exact flags:

```
arm-none-eabi-c++
  -std=c++11
  -mcpu=cortex-m7
  -mfpu=fpv5-d16
  -mfloat-abi=hard
  -mthumb
  -fno-rtti
  -fno-exceptions
  -Os
  -fPIC
  -I<path-to-include-folder>
  -c -o YourPlugin.o YourPlugin.cpp
```

The output is a single `.o` file. It is **not** linked — it is a relocatable object file loaded directly by the firmware. Place it in `/programs/plug-ins/` on the MicroSD card.

The project Makefile example:
```makefile
CC = arm-none-eabi-c++
CFLAGS = -std=c++11 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard \
         -mthumb -fno-exceptions -Os -fPIC -Wall -I$(NT_API_PATH)

YourPlugin.o: YourPlugin.cpp
    $(CC) $(CFLAGS) -c -o $@ $<
```

---

## Overall Plugin Architecture

A plugin file defines one or more **factories**. Each factory describes one algorithm (one entry in the Add Algorithm menu). The firmware discovers them through the `pluginEntry()` function, which every plugin must export.

The lifecycle of one algorithm instance:

```
pluginEntry()                    ← firmware scans the .o file at startup
  └─ calculateStaticRequirements()  ← reserve shared memory (lookup tables, etc.)
  └─ initialise()                   ← populate shared memory
  └─ calculateRequirements()        ← called when user browses "Add Algorithm"
  └─ construct()                    ← called when algorithm is added to preset
       └─ [audio starts]
            └─ parameterChanged()   ← called when any parameter value changes
            └─ step()               ← called every audio buffer (audio processing)
            └─ draw()               ← called when the module display needs updating
            └─ midiMessage()        ← called for incoming MIDI channel messages
            └─ midiRealtime()       ← called for incoming MIDI realtime bytes
```

---

## The pluginEntry() Function

This is the one required C-linkage export. The firmware calls it to discover what factories the plugin contains.

```cpp
extern "C" uintptr_t pluginEntry( _NT_selector selector, uint32_t data )
{
    switch ( selector )
    {
    case kNT_selector_version:
        return kNT_apiVersionCurrent;   // always return the API version you compiled against
    case kNT_selector_numFactories:
        return 1;                       // how many factories (algorithms) this .o provides
    case kNT_selector_factoryInfo:
        return (uintptr_t)( data == 0 ? &factory : NULL );  // return factory by index
    }
    return 0;
}
```

- `kNT_selector_version` — must return `kNT_apiVersionCurrent` (or the specific version you target).
- `kNT_selector_numFactories` — tells the firmware how many algorithms this file defines.
- `kNT_selector_factoryInfo` — `data` is the 0-based factory index; return a pointer to the matching `_NT_factory`.

For multiple factories in one file, return `&factory1`, `&factory2`, etc. based on `data`, and return `numFactories = 2` (etc.).

---

## The Algorithm Struct (Instance State)

Your algorithm's per-instance state lives in a struct that **publicly inherits from `_NT_algorithm`**:

```cpp
struct MyAlgorithm : public _NT_algorithm
{
    // Cached/derived values computed from parameters:
    float gain;
    float smoothingCoeff;

    // Running state (phase accumulators, filter state, etc.):
    float phase;
    float filterState;

    // Constructor — initialise all fields to safe defaults:
    MyAlgorithm()
    {
        parameters       = nullptr;   // will be set in construct()
        parameterPages   = nullptr;   // will be set in construct()
        vIncludingCommon = nullptr;   // managed by the firmware
        v                = nullptr;   // managed by the firmware
        gain             = 1.0f;
        phase            = 0.0f;
        filterState      = 0.0f;
    }
};
```

`_NT_algorithm` itself (from `api.h`) contains four fields you must not modify after construction:

| Field | Type | Description |
|---|---|---|
| `parameters` | `const _NT_parameter*` | Your parameter array — set in `construct()` |
| `parameterPages` | `const _NT_parameterPages*` | Your page layout — set in `construct()` |
| `vIncludingCommon` | `const int16_t*` | All parameter values including firmware-managed ones (bypass etc.) — **read-only, managed by firmware** |
| `v` | `const int16_t*` | Your own parameter values, starting at index 0 — **read-only, managed by firmware** |

**Never write to `v` or `vIncludingCommon`** — these are managed by the firmware. Read them in `step()` and `parameterChanged()`.

### Performance-Critical State: DTC Memory

If you have state that is accessed every sample in `step()`, put it in a separate struct allocated in DTC (tightly-coupled memory) for maximum performance:

```cpp
struct MyAlgorithm_DTC
{
    uint32_t phase;
    float    filterState;
};

struct MyAlgorithm : public _NT_algorithm
{
    MyAlgorithm( MyAlgorithm_DTC* dtc_ ) : dtc( dtc_ ) {}
    MyAlgorithm_DTC* dtc;
};
```

Allocate DTC by setting `req.dtc = sizeof(MyAlgorithm_DTC)` in `calculateRequirements()` and using `(MyAlgorithm_DTC*)ptrs.dtc` in `construct()`.

---

## Defining Parameters

### The _NT_parameter Struct

Each parameter is an `_NT_parameter`:

```cpp
struct _NT_parameter
{
    const char*             name;         // Display name
    int16_t                 min;          // Minimum value (signed 16-bit)
    int16_t                 max;          // Maximum value
    int16_t                 def;          // Default value
    uint8_t                 unit;         // Unit type (kNT_unit...)
    uint8_t                 scaling;      // Scaling (kNT_scaling...)
    char const * const *    enumStrings;  // Array of strings for kNT_unitEnum, else NULL
};
```

All parameters are **16-bit signed integers** internally. If you need float values, store an integer in the parameter and scale it in `parameterChanged()`.

### Parameter Units

| Constant | Display |
|---|---|
| `kNT_unitNone` | Raw integer, no unit label |
| `kNT_unitEnum` | Shows string from `enumStrings` array |
| `kNT_unitDb` | dB |
| `kNT_unitDb_minInf` | dB, minimum shown as "−inf" |
| `kNT_unitPercent` | % |
| `kNT_unitHz` | Hz |
| `kNT_unitSemitones` | semitones |
| `kNT_unitCents` | cents |
| `kNT_unitMs` | ms |
| `kNT_unitSeconds` | seconds |
| `kNT_unitFrames` | frames |
| `kNT_unitMIDINote` | MIDI note number |
| `kNT_unitMillivolts` | mV |
| `kNT_unitVolts` | V |
| `kNT_unitBPM` | BPM |
| `kNT_unitDegrees` | ° |
| `kNT_unitHasStrings` | Calls `parameterString()` for custom display string |
| `kNT_unitConfirm` | User must confirm changes; also calls `parameterString()` |

The following units are used **only via the I/O macros** (see below):

| Constant | Meaning |
|---|---|
| `kNT_unitAudioInput` | Bus selector for audio input |
| `kNT_unitCvInput` | Bus selector for CV input |
| `kNT_unitAudioOutput` | Bus selector for audio output |
| `kNT_unitCvOutput` | Bus selector for CV output |
| `kNT_unitOutputMode` | Add/Replace output mode toggle |

### Scaling

Scaling is a display hint — it divides the integer value by 10, 100, or 1000 for display only. The raw `int16_t` is always what `v[]` contains.

| Constant | Divides displayed value by |
|---|---|
| `kNT_scalingNone` | 1 (no scaling) |
| `kNT_scaling10` | 10 |
| `kNT_scaling100` | 100 |
| `kNT_scaling1000` | 1000 |

### Enum Parameters

For enum parameters, provide a `NULL`-terminated array of `const char*`:

```cpp
static const char* const waveformStrings[] = { "Sine", "Square", "Saw", NULL };

static const _NT_parameter paramWaveform = {
    .name        = "Waveform",
    .min         = 0,
    .max         = 2,           // number of options minus 1
    .def         = 0,
    .unit        = kNT_unitEnum,
    .scaling     = kNT_scalingNone,
    .enumStrings = waveformStrings
};
```

### I/O Bus Parameters (Routing)

Use the provided macros to define inputs and outputs. These create parameters that let the user choose which bus (1–64) the algorithm connects to:

```cpp
NT_PARAMETER_AUDIO_INPUT( "Input", 1, 1 )
// → min=1, max=kNT_lastBus(64), def=1, unit=kNT_unitAudioInput

NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output", 1, 13 )
// → Creates TWO parameters: the output bus selector AND an output mode parameter

NT_PARAMETER_CV_INPUT( "CV In", 0, 0 )
// → min=0 means "no input allowed" as default (0 = disconnected)

NT_PARAMETER_CV_OUTPUT_WITH_MODE( "CV Out", 1, 13 )
```

`NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE` and `NT_PARAMETER_CV_OUTPUT_WITH_MODE` each expand to **two** parameter definitions — one for the bus number and one for the output mode (Add/Replace). Account for this in your parameter enum indices.

Available macros:
- `NT_PARAMETER_AUDIO_INPUT(name, min, default)`
- `NT_PARAMETER_CV_INPUT(name, min, default)`
- `NT_PARAMETER_AUDIO_OUTPUT(name, min, default)` — bus only, no mode
- `NT_PARAMETER_CV_OUTPUT(name, min, default)` — bus only, no mode
- `NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE(name, min, default)` — bus + mode (2 params)
- `NT_PARAMETER_CV_OUTPUT_WITH_MODE(name, min, default)` — bus + mode (2 params)

**Convention: default output bus is 13** (the first physical output bus, which is bus 13 in the 64-bus system where 1–12 are inputs). Default input bus is 1.

### Parameter Array and Index Enum

Define all parameters in a static array and number them with an enum. The enum index is exactly what `v[]` uses:

```cpp
// Indices — must match the order of entries in the parameters[] array exactly
enum
{
    kParamInput,        // 0
    kParamOutput,       // 1
    kParamOutputMode,   // 2  (created by _WITH_MODE macro — comes immediately after Output)
    kParamGain,         // 3
    kParamMidiChannel,  // 4
};

static const _NT_parameter parameters[] = {
    NT_PARAMETER_AUDIO_INPUT( "Input", 1, 1 )           // index 0
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output", 1, 13 )  // index 1 + 2
    { .name="Gain",  .min=0, .max=100, .def=50, .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },  // 3
    { .name="MIDI Ch", .min=1, .max=16, .def=1, .unit=kNT_unitNone,    .scaling=0, .enumStrings=NULL },  // 4
};
```

**`_WITH_MODE` macros expand to TWO entries.** In the LASER plugin this is handled by using individual `_NT_parameter` structs combined into an `allParams[]` array with comment-annotated index numbers:

```cpp
static const _NT_parameter allParams[] = {
    paramFreq,       //  0
    paramRotX,       //  1
    paramRotY,       //  2
    // ... etc
};
```

This pattern (define individually, combine with numbered comments) is recommended for larger plugins to keep indexing clear.

---

## Defining Parameter Pages

Pages control how parameters are grouped and displayed on the module's screen. Each page has a name and lists which parameter indices belong on it.

```cpp
// Index arrays — list which parameter indices appear on each page
static const uint8_t page1[] = { kParamGain, kParamMidiChannel };
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

The `group` field: if two pages share the same non-zero group number, switching between them preserves the cursor position. If `group=0`, it defaults to the page's own index (no sharing).

**The LASER plugin uses a shorthand** omitting `.group` (defaults to 0) and `.unused[]`:
```cpp
static const _NT_parameterPage pages[] = {
    { "Frequency", 2, page1_indices },  // name, numParams, params — group defaults to 0
    { "Rotation",  3, page2_indices },
};
```

Both styles are valid.

---

## Memory Allocation: calculateRequirements() and construct()

### calculateRequirements()

Called by the firmware before constructing an instance. Tells the firmware how much memory to allocate.

```cpp
void calculateRequirements( _NT_algorithmRequirements& req, const int32_t* specifications )
{
    req.numParameters = ARRAY_SIZE(parameters);  // number of your parameters
    req.sram = sizeof(MyAlgorithm);              // for the instance struct
    req.dram = 0;                                // large buffers (e.g. delay lines)
    req.dtc  = sizeof(MyAlgorithm_DTC);          // performance-critical per-sample data
    req.itc  = 0;                                // for code only (rarely needed)
}
```

Memory types:
- **sram** — general SRAM; use for the algorithm instance struct
- **dram** — external DRAM; use for large allocations (delay buffers, sample data)
- **dtc** — Data Tightly Coupled memory; fastest memory, use for data accessed every sample
- **itc** — Instruction Tightly Coupled; for code only (very rarely needed)

`specifications` is an array of `int32_t` specification values (see Specifications section).

### construct()

Called to create the instance. Use **placement new** to construct your struct in the provided memory:

```cpp
_NT_algorithm* construct( const _NT_algorithmMemoryPtrs& ptrs,
                          const _NT_algorithmRequirements& req,
                          const int32_t* specifications )
{
    // Place your algorithm struct in the provided SRAM block
    MyAlgorithm* alg = new (ptrs.sram) MyAlgorithm();

    // REQUIRED: set the parameter and page pointers
    alg->parameters     = parameters;
    alg->parameterPages = &parameterPages;

    return alg;
}
```

If you have DTC memory:
```cpp
_NT_algorithm* construct( const _NT_algorithmMemoryPtrs& ptrs, ... )
{
    MyAlgorithm* alg = new (ptrs.sram) MyAlgorithm( (MyAlgorithm_DTC*)ptrs.dtc );
    alg->parameters     = parameters;
    alg->parameterPages = &parameterPages;
    return alg;
}
```

---

## Shared Memory: calculateStaticRequirements() and initialise()

Used for data shared across all instances of a plugin (e.g. lookup tables, normalised geometry).

```cpp
void calculateStaticRequirements( _NT_staticRequirements& req )
{
    req.dram = 256;  // bytes of shared DRAM needed
}

static float* sharedTable = nullptr;

void initialise( _NT_staticMemoryPtrs& ptrs, const _NT_staticRequirements& /*req*/ )
{
    sharedTable = (float*)ptrs.dram;
    // populate sharedTable here — runs once at load time
}
```

These are optional. If your plugin has no shared data, set both to `NULL` in the factory.

---

## parameterChanged()

Called by the firmware whenever any parameter value changes (user turns a knob, preset loads, CV mapping fires, etc.). Use this to compute and cache derived values so `step()` stays fast.

```cpp
void parameterChanged( _NT_algorithm* self, int p )
{
    MyAlgorithm* pThis = (MyAlgorithm*)self;

    // p is the 0-based index into your parameters[] array
    switch ( p )
    {
    case kParamGain:
        pThis->gain = pThis->v[kParamGain] / 100.0f;
        break;

    case kParamSmoothingMs:
        {
            float ms = pThis->v[kParamSmoothingMs];
            float sampleRate = (float)NT_globals.sampleRate;
            pThis->smoothingCoeff = 1.0f - expf( -1.0f / (ms * 0.001f * sampleRate) );
        }
        break;
    }
}
```

**Key points:**
- `p` is the index into your own `parameters[]` array (same as the enum values).
- `self->v[p]` contains the new `int16_t` value.
- `NT_globals.sampleRate` is always valid here.
- This is safe to call `NT_setParameterFromAudio()` from.
- Do NOT do heavy computation here that would cause dropouts — keep it lean.

---

## step() — The Audio Processing Function

This is the real-time audio callback. Called once per buffer. **It must never block, allocate memory, or take an unpredictable amount of time.**

```cpp
void step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
    MyAlgorithm* pThis = (MyAlgorithm*)self;

    int numFrames = numFramesBy4 * 4;  // actual number of samples to process

    // Access busses: busFrames is a flat array.
    // Bus N (1-based, as stored in v[]) maps to:
    //   float* bus = busFrames + (N - 1) * numFrames;

    const float* in  = busFrames + (pThis->v[kParamInput]  - 1) * numFrames;
    float*       out = busFrames + (pThis->v[kParamOutput] - 1) * numFrames;
    bool         replace = pThis->v[kParamOutputMode];  // 0=Add, 1=Replace

    float gain = pThis->gain;  // pre-computed in parameterChanged()

    for ( int i = 0; i < numFrames; ++i )
    {
        float processed = in[i] * gain;

        if ( !replace )
            out[i] += processed;   // Add mode: sum onto existing bus content
        else
            out[i] = processed;    // Replace mode: overwrite bus content
    }
}
```

### Bus Layout in Memory

`busFrames` is a contiguous array of `numFrames` floats per bus, all 64 busses laid out consecutively:

```
busFrames[0 .. numFrames-1]           → bus 1  (physical input 1)
busFrames[numFrames .. 2*numFrames-1] → bus 2  (physical input 2)
...
busFrames[12*numFrames .. ...]        → bus 13 (first physical output)
...
busFrames[63*numFrames .. ...]        → bus 64 (last aux bus)
```

Bus numbers in parameters are **1-based**. Subtract 1 when indexing `busFrames`.

### Add vs Replace Output Mode

The `kNT_unitOutputMode` parameter (created by `_WITH_MODE` macros) stores:
- `0` = Add: `out[i] += value` — your signal is mixed/summed onto the bus
- `1` = Replace: `out[i] = value` — your signal replaces all previous content on that bus

Always respect this. Source algorithms (instruments generating signals from nothing) typically default to Add. Effects that process a signal and write it to the same bus use Replace.

### The NT_globals Structure

Always available in `step()`, `parameterChanged()`, `midiMessage()`, and `midiRealtime()`:

```cpp
extern const _NT_globals NT_globals;

// NT_globals.sampleRate          — e.g. 48000
// NT_globals.maxFramesPerStep    — maximum frames per step() call
// NT_globals.workBuffer          — scratch buffer (does NOT persist between calls)
// NT_globals.workBufferSizeBytes — size of the scratch buffer in bytes
```

**`NT_globals.workBuffer`** is a pre-allocated scratch buffer you can use freely inside `step()` for intermediate calculations. It is not cleared between calls and is not yours to keep — do not store pointers to it.

---

## The Factory Struct (_NT_factory)

The factory is a static const struct that ties everything together. It is what `pluginEntry()` returns.

```cpp
static const _NT_factory factory =
{
    .guid                        = NT_MULTICHAR('Y','o','u','R'),  // 4-char unique ID
    .name                        = "My Algorithm",                 // Name in the menu
    .description                 = "Does something useful",

    .numSpecifications           = 0,       // number of specifications (0 if none)
    .specifications              = NULL,    // pointer to specs array (NULL if none)

    // Shared memory (optional — set to NULL if not needed):
    .calculateStaticRequirements = calculateStaticRequirements,
    .initialise                  = initialise,

    // Per-instance memory and construction (required):
    .calculateRequirements       = calculateRequirements,
    .construct                   = construct,

    // Callbacks (all optional — set to NULL if not used):
    .parameterChanged            = parameterChanged,
    .step                        = step,
    .draw                        = NULL,
    .midiRealtime                = NULL,
    .midiMessage                 = midiMessage,
    .tags                        = kNT_tagEffect,

    // Custom UI (optional):
    .hasCustomUi                 = NULL,
    .customUi                    = NULL,
    .setupUi                     = NULL,

    // Serialisation (optional):
    .serialise                   = NULL,
    .deserialise                 = NULL,

    // SysEx (optional):
    .midiSysEx                   = NULL,

    // Parameter display helpers (optional):
    .parameterUiPrefix           = NULL,
    .parameterString             = NULL,
};
```

**Only `construct` is strictly required.** All other function pointers can be `NULL`. For fields that appear in newer API versions but you don't need, set them to `NULL`.

### GUID Rules

- Use `NT_MULTICHAR('A','B','C','D')` — four ASCII characters packed into a `uint32_t`.
- **Must contain at least one capital letter.** All-lowercase GUIDs are reserved for built-in algorithms.
- Must be globally unique. Check [NT Gallery](https://nt-gallery.nosuch.dev) to avoid collisions.

```cpp
.guid = NT_MULTICHAR('P','O','L','Y')  // OK — uppercase letters
.guid = NT_MULTICHAR('E','x','g','a')  // OK — mixed case
.guid = NT_MULTICHAR('g','a','i','n')  // WRONG — all lowercase, reserved
```

### Tags

Tags categorise your plugin in the menu. OR them together if multiple apply:

```cpp
.tags = kNT_tagEffect | kNT_tagReverb    // it's a reverb effect
.tags = kNT_tagInstrument                // it's a synth
.tags = kNT_tagUtility                   // it's a utility
```

| Tag | Use for |
|---|---|
| `kNT_tagInstrument` | Synthesisers, sound generators |
| `kNT_tagEffect` | Audio effects (general) |
| `kNT_tagReverb` | Reverb effects |
| `kNT_tagDelay` | Delay effects |
| `kNT_tagUtility` | Utilities, converters, routers |
| `kNT_tagFilterEQ` | Filters and equalisers |

---

## Specifications (Optional)

Specifications are algorithm-level settings chosen **at instantiation time** (before the algorithm starts running). They are similar to parameters but their values affect memory allocation — changing them requires rebuilding the algorithm instance (audio interruption).

Use specifications for things like: maximum delay time, number of channels, buffer size.

```cpp
static const _NT_specification specifications[] = {
    {
        .name = "Max delay (s)",
        .min  = 1,
        .max  = 10,
        .def  = 2,
        .type = kNT_typeSeconds,   // kNT_typeGeneric, kNT_typeSeconds, or kNT_typeBoolean
    },
};

// In calculateRequirements, use the spec values:
void calculateRequirements( _NT_algorithmRequirements& req, const int32_t* specifications )
{
    int maxDelaySamples = specifications[0] * 48000;  // assuming 48kHz
    req.numParameters = ARRAY_SIZE(parameters);
    req.sram = sizeof(MyAlgorithm);
    req.dram = maxDelaySamples * sizeof(float);       // allocate based on spec
    req.dtc  = 0;
    req.itc  = 0;
}

// In factory:
.numSpecifications = ARRAY_SIZE(specifications),
.specifications    = specifications,
```

The `specifications` pointer passed to `calculateRequirements()` and `construct()` contains the user's chosen values at the time of instantiation.

---

## MIDI Callbacks

### midiMessage()

Called for standard channel messages (0x80–0xE0):

```cpp
void midiMessage( _NT_algorithm* self, uint8_t byte0, uint8_t byte1, uint8_t byte2 )
{
    MyAlgorithm* pThis = (MyAlgorithm*)self;

    int status  = byte0 & 0xF0;   // message type
    int channel = byte0 & 0x0F;   // MIDI channel (0–15)

    switch ( status )
    {
    case 0x90:  // Note On
        if ( byte2 == 0 ) goto noteOff;  // velocity 0 = note off
        // handle note on: byte1 = note, byte2 = velocity
        break;
    case 0x80:  // Note Off
    noteOff:
        // handle note off: byte1 = note, byte2 = velocity
        break;
    case 0xB0:  // Control Change
        // byte1 = CC number, byte2 = value
        break;
    }
}
```

### midiRealtime()

Called for realtime bytes (clock, start, stop, continue). Runs at highest priority:

```cpp
void midiRealtime( _NT_algorithm* self, uint8_t byte )
{
    // byte = 0xF8 (clock), 0xFA (start), 0xFB (continue), 0xFC (stop)
}
```

### Sending MIDI

From within `step()`, `parameterChanged()`, `midiRealtime()`, or `midiMessage()`:

```cpp
// destination: logical OR of kNT_destinationBreakout, kNT_destinationSelectBus, kNT_destinationUSB, kNT_destinationInternal
NT_sendMidi3ByteMessage( kNT_destinationUSB, 0xB0, ccNumber, ccValue );
NT_sendMidi2ByteMessage( kNT_destinationBreakout, 0xD0, pressure );
NT_sendMidiByte( kNT_destinationSelectBus, 0xF8 );  // clock
```

---

## draw() — Custom Display

Called when the module display needs updating. Return `false` to also show the standard parameter line at the top; return `true` to suppress it.

```cpp
bool draw( _NT_algorithm* self )
{
    // NT_screen is 256x64 pixels, 4bpp (2 pixels per byte), 128*64 bytes total
    // Pixel layout: each byte = two horizontally adjacent pixels, high nibble = left pixel

    // Draw text
    NT_drawText( 10, 20, "Hello", 15, kNT_textLeft, kNT_textNormal );
    //           x   y   text    colour (0–15)  alignment   size

    // Draw shapes (integer, not antialiased)
    NT_drawShapeI( kNT_line, x0, y0, x1, y1, colour );
    NT_drawShapeI( kNT_rectangle, x0, y0, x1, y1, colour );
    NT_drawShapeI( kNT_box, x0, y0, x1, y1, colour );

    // Draw shapes (float, antialiased)
    NT_drawShapeF( kNT_line, fx0, fy0, fx1, fy1, fcolour );

    // String helpers (write to a char buffer):
    char buf[32];
    NT_intToString( buf, intValue );
    NT_floatToString( buf, floatValue, 2 );  // 2 decimal places

    return false;  // true = suppress standard header line
}
```

Text sizes: `kNT_textTiny` (3×5px), `kNT_textNormal` (8pt bitmap), `kNT_textLarge` (21pt antialiased).
Text alignment: `kNT_textLeft`, `kNT_textCentre`, `kNT_textRight`.
Shapes: `kNT_point`, `kNT_line`, `kNT_box` (unfilled), `kNT_rectangle` (filled), `kNT_circle` (unfilled).

**Only call draw functions from within `draw()`.** Do not call them from `step()` or anywhere else.

---

## Setting Parameters Programmatically

From within `step()`, `parameterChanged()`, `midiRealtime()`, or `midiMessage()`:

```cpp
// Get this algorithm's index in the preset
int32_t myIndex = NT_algorithmIndex( self );

// Set a parameter (audio-thread safe)
NT_setParameterFromAudio( myIndex, parameterIndex + NT_parameterOffset(), newValue );

// Gray out a parameter (show it as inactive)
NT_setParameterGrayedOut( myIndex, parameterIndex + NT_parameterOffset(), true );
```

From UI code (not from audio thread):
```cpp
NT_setParameterFromUi( myIndex, parameterIndex + NT_parameterOffset(), newValue );
```

`NT_parameterOffset()` returns the offset between your plugin's 0-based parameter indices and the global parameter numbering used by `NT_setParameter*`. Always add it.

If a parameter definition changes at runtime (e.g. you update `self->parameters[p].name`):
```cpp
NT_updateParameterDefinition( NT_algorithmIndex(self), parameterIndex );
```

---

## Accessing Other Algorithms (NT_slot)

Include `distingnt/slot.h` to inspect other algorithms in the preset:

```cpp
#include <distingnt/slot.h>

// Get number of algorithms in preset
uint32_t count = NT_algorithmCount();

// Access a specific algorithm slot
_NT_slot slot;
if ( NT_getSlot( slot, index ) )  // index is 0-based
{
    const char*     name  = slot.name();
    uint32_t        guid  = slot.guid();
    _NT_algorithm*  plug  = slot.plugin();  // NULL if it's a built-in algo
    int16_t         value = slot.parameterValue( paramIndex );
    int16_t         preset = slot.parameterPresetValue( paramIndex );
}
```

---

## parameterString() — Custom Value Display

Implement this when a parameter uses `kNT_unitHasStrings` or `kNT_unitConfirm`:

```cpp
int parameterString( _NT_algorithm* self, int p, int v, char* buff )
{
    switch ( p )
    {
    case kParamMyCustomParam:
        {
            float val = v / 1000.0f;
            return NT_floatToString( buff, val, 3 );  // return string length
        }
    }
    return 0;  // 0 = use default numeric display
}
```

Buffer size is guaranteed to be at least `kNT_parameterStringSize` (64) bytes.

---

## Complete Minimal Plugin Example

This is the absolute minimum structure for a working plugin (a pass-through gain):

```cpp
#include "distingnt/api.h"
#include <new>

// --- Instance state ---
struct GainAlgorithm : public _NT_algorithm
{
    float gain;
    GainAlgorithm() : gain( 1.0f ) {}
};

// --- Parameters ---
enum { kParamIn, kParamOut, kParamOutMode, kParamGain };

static const _NT_parameter parameters[] = {
    NT_PARAMETER_AUDIO_INPUT( "Input", 1, 1 )
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output", 1, 13 )
    { .name="Gain", .min=0, .max=100, .def=100,
      .unit=kNT_unitPercent, .scaling=0, .enumStrings=NULL },
};

// --- Pages ---
static const uint8_t page1[] = { kParamGain };
static const uint8_t page2[] = { kParamIn, kParamOut, kParamOutMode };
static const _NT_parameterPage pages[] = {
    { .name="Gain",    .numParams=1, .group=0, .params=page1 },
    { .name="Routing", .numParams=3, .group=0, .params=page2 },
};
static const _NT_parameterPages paramPages = { .numPages=2, .pages=pages };

// --- Lifecycle ---
void calculateRequirements( _NT_algorithmRequirements& req, const int32_t* )
{
    req.numParameters = ARRAY_SIZE(parameters);
    req.sram = sizeof(GainAlgorithm);
    req.dram = req.dtc = req.itc = 0;
}

_NT_algorithm* construct( const _NT_algorithmMemoryPtrs& ptrs,
                          const _NT_algorithmRequirements&,
                          const int32_t* )
{
    GainAlgorithm* alg = new (ptrs.sram) GainAlgorithm();
    alg->parameters    = parameters;
    alg->parameterPages = &paramPages;
    return alg;
}

void parameterChanged( _NT_algorithm* self, int p )
{
    GainAlgorithm* a = (GainAlgorithm*)self;
    if ( p == kParamGain )
        a->gain = a->v[kParamGain] / 100.0f;
}

void step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
    GainAlgorithm* a  = (GainAlgorithm*)self;
    int   n           = numFramesBy4 * 4;
    const float* in   = busFrames + (a->v[kParamIn]  - 1) * n;
    float*       out  = busFrames + (a->v[kParamOut] - 1) * n;
    bool         rep  = a->v[kParamOutMode];
    float        gain = a->gain;

    for ( int i = 0; i < n; ++i )
        out[i] = rep ? in[i] * gain : out[i] + in[i] * gain;
}

// --- Factory ---
static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('M','y','G','n'),
    .name                        = "MyGain",
    .description                 = "Simple gain plugin",
    .numSpecifications           = 0,
    .specifications              = NULL,
    .calculateStaticRequirements = NULL,
    .initialise                  = NULL,
    .calculateRequirements       = calculateRequirements,
    .construct                   = construct,
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

// --- Entry point ---
extern "C" uintptr_t pluginEntry( _NT_selector selector, uint32_t data )
{
    switch ( selector ) {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return (uintptr_t)(data==0 ? &factory : NULL);
    }
    return 0;
}
```

---

## Rules Summary

| Rule | Detail |
|---|---|
| Target hardware | ARM Cortex-M7, not a computer |
| No heap | No `new`/`delete`/`malloc`/`free` except placement new on firmware-provided memory |
| No exceptions | Compiler flag `-fno-exceptions` — do not use `try`/`catch`/`throw` |
| No RTTI | Compiler flag `-fno-rtti` — do not use `dynamic_cast` or `typeid` |
| No STL containers | No `std::vector`, `std::string`, `std::map`, etc. |
| Parameters are int16_t | Always 16-bit signed integers. Scale in `parameterChanged()` |
| Bus indices are 1-based | Subtract 1 when indexing `busFrames` in `step()` |
| Output modes | Always implement Add (`+=`) and Replace (`=`) based on the output mode parameter |
| GUID uniqueness | At least one capital letter; globally unique |
| step() is real-time | Never block, never allocate, never do I/O |
| `v[]` is read-only | Never write to `self->v[]` or `self->vIncludingCommon[]` |
| API version | Return `kNT_apiVersionCurrent` from `pluginEntry()` unless targeting a specific older version |
| Static data is shared | `static` local arrays in `step()` are shared across all instances and across calls — be careful |

---

## Development Workflow

1. Write your `.cpp` file.
2. Compile with `arm-none-eabi-c++` using the required flags → produces `.o`.
3. Copy the `.o` to `/programs/plug-ins/` on the MicroSD card.
4. The module scans plugins on startup (or on card remount).
5. For rapid iteration during development: use the SysEx rescan command (`7AH / 08H`) to rescan plugins without rebooting.
6. Check `Misc / Plug-ins / View info…` on the module to see if your plugin loaded or failed, and why.

---

## Cross-Reference with Disting NT Infos

| Concept | Details |
|---|---|
| Bus numbers | Physical inputs: 1–12. Physical outputs: 13–20. Aux: 21–64. Total: 64 busses. |
| Default output bus | 13 (first physical output) |
| Default sample rate | 48 kHz; check `NT_globals.sampleRate` at runtime — never hardcode |
| CPU limit | Stay below 90% total. Bypass → 0 CPU. |
| Plugin location | `/programs/plug-ins/` on MicroSD (FAT32 or exFAT) |
| Max algorithms | 32 per preset |
| Output voltage range | ±11V (outputs). Design your signals accordingly. Eurorack audio typically ±5V; CV typically 0–5V or ±5V. |
