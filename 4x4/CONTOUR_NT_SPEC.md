# Contour: build brief for a Disting NT plugin

This file is a complete brief for implementing **Contour**, a random function generator, as part of a Disting NT plugin written in C++ against the Expert Sleepers `distingNT_API` only.

It contains:

1. what Contour does, precisely enough to reimplement it;
2. how it maps onto the Disting NT API (parameters, pages, I/O, `step()`, `draw()`, memory);
3. a reference engine in C++ (section 9), already verified against the original web prototype;
4. acceptance tests with exact expected voltages.

Use the reference engine as given unless something in it conflicts with the API. If you change its behaviour, the golden tests in section 8 must still pass.

## 1. Hard constraints

- **Disting NT API only.** Include `<distingnt/api.h>`. The only other headers allowed are `<math.h>`, `<stdint.h>` and `<new>` (placement new, exactly as the API's own examples use it). No `<algorithm>`, no STL containers, no third-party code. The reference engine already meets this.
- **No heap.** All state lives in the algorithm object, constructed with placement new into the memory the host provides in `construct()`.
- **Build like the API examples:** `arm-none-eabi-c++ -std=c++11 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -fno-rtti -fno-exceptions -Os -fPIC -Wall`. The engine uses `double` maths; the Cortex-M7 FPU (`fpv5-d16`) does double precision in hardware.
- **Output is in volts.** Disting NT busses carry volts (1.0 = 1 V; the API's own examples treat a bus value above 1.0 as a high gate). Contour's output ranges up to ±10 V.
- **"Part of a plugin".** Contour may be its own algorithm (its own `_NT_factory`) or a section inside an existing plugin. Section 4 describes it as a standalone algorithm. To embed it in an existing plugin instead, add the same parameters as an extra page, add one `contour::Engine` member to that plugin's algorithm struct, and run the same per-sample code inside its `step()`. Ask the user which they want if it is not clear from the project.

## 2. What Contour does

Contour generates a one-shot control voltage function lasting 1 to 10 seconds. It can be retriggered or looped.

1. The length is cut into **W equal windows**. Each window holds **two equal envelopes**.
2. Each envelope has two stages: move toward level **L1** at rate **R1**, then toward **L2** at rate **R2**, then stay at L2 until the envelope's time is up. Levels are 0–99. Movement is linear in level.
3. A new envelope **starts from wherever the level currently is**. It does not reset, like retriggering a DX7 envelope.
4. **Rates.** Every rate starts at 0, the slowest. **Chaos** (0–100%) pulls each rate and each level toward its own random target in 0–99. At 0% everything crawls; at 100% rates and levels are fully random.
5. The level becomes a voltage: `0 → Level` (unipolar) or `±Level` (bipolar), plus Offset, clipped at ±10 V.
6. **Hold prob** places random regions covering that share of the length. Inside a region the output freezes at its value on entry, then jumps back to the live value on exit.
7. **Round prob** places random regions covering that share of the length. Every corner inside one is rounded with a **Radius**: the corner is replaced by a parabola running from one radius before the corner to one radius after it, tangent to both straight segments. There is no lag. A jump at the end of a hold, if inside a rounding region, becomes an S-curve across the same span.
8. Everything random derives from one **Seed**. The same seed and settings always give the same function. **Reseed** picks a new seed.

## 3. Algorithm details

Section 9 has the code; this section is the explanation.

### 3.1 Random numbers

All randomness comes from `rnd(i, j)`, a value in [0, 1). It is one draw of mulberry32 seeded with

```
uint32( seed*100003 + i*7919 + j*104729 )      computed in 64-bit, truncated to 32-bit
```

It is a keyed lookup, not a sequence, so independent decisions never disturb each other. The key ranges are:

| `i` | `j` | Used for |
|---|---|---|
| 2000 + envelope index | 2, 4 | base levels L1, L2 (`99 × rnd`) |
| 3000 + envelope index | 1, 2, 3, 4 | chaos targets for R1, L1, R2, L2 |
| 500000 + k | 1, 2, 3 | rounding regions (piece size, visit order, offset) |
| 600000 + k | 1, 2, 3 | hold regions |

### 3.2 Envelope values

For each envelope, with base rate 0 and base level `99 × rnd(2000+idx, j)`:

```
value = floor( base + chaos × (99 × rnd(3000+idx, j) − base) + 0.5 )
```

### 3.3 Rate to time

A rate R crosses the full 0–99 range in `20 × 2^(−R/7.5)` seconds: 20 s at rate 0, about 0.2 s at 50, about 2 ms at 99.

### 3.4 Random regions (used for both Hold and Round)

1. Cut the length into 24 pieces with weights `0.25 + rnd(base+k, 1)`.
2. Visit the pieces in ascending order of `rnd(base+k, 2)`.
3. Take whole pieces until the next one would exceed `prob × length`.
4. Take part of that last piece, offset by `rnd(base+k, 3) × (pieceLength − remaining)`, so the total is exact.
5. Sort by start time and merge pieces that touch.

`prob ≥ 1` gives one region over everything; `prob ≤ 0` gives none.

### 3.5 Building the curve

This happens at each trigger and after each parameter change; it is never done per sample.

1. **Level polyline.** Simulate the envelopes exactly, starting from the start level. Emit a point at each envelope start, each stage completion, and each envelope end where a stage was cut off.
2. **Volts.** Map each point to volts and clip at ±10 V, inserting a point wherever a segment crosses a rail.
3. **Holds.** Replace each hold region `[a, b]` with a point at `a` holding the value there, and a point at `b` whose left value is the held value and whose right value is the live value (a jump).
4. **Radii.** For each interior point, record the incoming slope `s1` and outgoing slope `s2`. If the point lies inside a rounding region, set its radius to `min(Radius, gapBefore/2, gapAfter/2)` so neighbouring roundings can never overlap.

The start level is the last envelope's L2 the first time. After that it is the envelope level at the moment of the trigger.

### 3.6 Evaluating a sample

Find the segment `[A, B]` containing time t (a forward-moving index; it never searches backward during playback).

- Default: straight line, `A.vr + B.s1 × (t − A.t)`.
- Within `B.r` before B, with `u = (t − (B.t − B.r)) / (2·B.r)`:
  `line + (B.s2 − B.s1)·B.r·u² + (B.vr − B.vl)·u²(3 − 2u)`
- Within `A.r` after A: the same formula centred on A, with the incoming line extended forward.

This is a quadratic blend tangent to both segments, plus a smoothstep for any jump.

### 3.7 Playback

- `trigger()` builds the curve from the current level and starts at time 0.
- The function ends at its length. With Loop on, it retriggers at the end; with Loop off, the output holds the last value.
- `rebuild()` (after a parameter change) re-plans the current pass from the level that pass started at, and keeps the playback position. Turning a knob reshapes the curve in place; it does not restart it.

## 4. Disting NT integration

### 4.1 Factory

- `guid`: four characters with at least one capital letter, for example `NT_MULTICHAR('B','z','C','t')`. Confirm with the user; the user's existing plugins use BZMS and CHRD.
- `name`: "Contour". `description`: "Random rate/level function generator, 1-10 s".
- No specifications. Tag `kNT_tagUtility`.
- Implement `calculateRequirements`, `construct`, `parameterChanged`, `step` and `draw`. `serialise` and `deserialise` are not needed: every setting is a parameter, so presets save it.

### 4.2 Parameters

Values are `int16_t`. Use `.scaling = kNT_scaling10` where the table shows a decimal.

| # | Name | min | max | def | unit | scaling | Engine mapping |
|---|---|---|---|---|---|---|---|
| 0 | Length | 10 | 100 | 60 | `kNT_unitSeconds` | `kNT_scaling10` | `lengthSec = v/10` |
| 1 | Windows | 1 | 32 | 6 | `kNT_unitNone` | 0 | `windows = v` |
| 2 | Chaos | 0 | 100 | 60 | `kNT_unitPercent` | 0 | `chaos = v/100` |
| 3 | Radius | 0 | 500 | 120 | `kNT_unitMs` | 0 | `radiusSec = v/1000` |
| 4 | Round prob | 0 | 100 | 100 | `kNT_unitPercent` | 0 | `roundProb = v/100` |
| 5 | Hold prob | 0 | 100 | 0 | `kNT_unitPercent` | 0 | `holdProb = v/100` |
| 6 | Level | 0 | 100 | 80 | `kNT_unitVolts` | `kNT_scaling10` | `levelV = v/10` |
| 7 | Offset | −50 | 50 | 0 | `kNT_unitVolts` | `kNT_scaling10` | `offsetV = v/10` |
| 8 | Bipolar | 0 | 1 | 0 | `kNT_unitEnum` {"Off","On"} | 0 | `bipolar = v` |
| 9 | Loop | 0 | 1 | 1 | `kNT_unitEnum` {"Off","On"} | 0 | `loop = v` |
| 10 | Seed | 1 | 32767 | 7 | `kNT_unitNone` | 0 | `seed = v` |
| 11 | Trigger input | 0 | `kNT_lastBus` | 0 | `NT_PARAMETER_CV_INPUT("Trigger input", 0, 0)` | | |
| 12 | Reseed input | 0 | `kNT_lastBus` | 0 | `NT_PARAMETER_CV_INPUT("Reseed input", 0, 0)` | | |
| 13 | Output | 1 | `kNT_lastBus` | 13 | `NT_PARAMETER_CV_OUTPUT_WITH_MODE("Output", 1, 13)` | | |
| 14 | Output mode | 0 | 1 | 0 | (added by the macro above) | | add / replace |

Notes:
- **Loop defaults to On** so that a freshly added Contour runs with no trigger patched. This differs from the web prototype, where Loop starts off.
- **Seed** is limited to the `int16_t` range. The web prototype's Reseed can produce seeds up to 1,000,000, so only seeds of 32767 or below can be copied over and reproduced exactly.

### 4.3 Pages

| Page | Parameters |
|---|---|
| Contour | Length, Windows, Chaos, Seed |
| Shape | Radius, Round prob, Hold prob |
| Output | Level, Offset, Bipolar, Loop |
| Routing | Trigger input, Reseed input, Output, Output mode |

### 4.4 Memory and construction

- The algorithm struct derives from `_NT_algorithm` and holds one `contour::Engine` (about 47 KB on a 64-bit desktop build; measure `sizeof` on the target), plus the small state below.
- `calculateRequirements`: `req.numParameters = ARRAY_SIZE(parameters)`, `req.sram = sizeof(ContourAlgorithm)`, other regions 0. If the host cannot provide that much SRAM, move the engine into `dram` (construct it with placement new at `ptrs.dram` and keep a pointer).
- `construct`: placement-new the struct at `ptrs.sram`, set `parameters` and `parameterPages`, then call `engine.setSampleRate((float)NT_globals.sampleRate)`. Do not rebuild here; the parameter values are not guaranteed to be set yet. Set a `dirty` flag instead.

Extra state in the algorithm struct:

```cpp
bool  dirty;            // parameters changed; re-plan at the next step()
bool  started;          // first trigger done
bool  trigHigh, reseedHigh;   // edge detectors (hysteresis)
bool  displayDirty;     // refresh the display cache
float display[256];     // curve sampled for draw()
float displayLo, displayHi;
```

### 4.5 `parameterChanged(self, p)`

- For parameters 0–10: copy the mapped value into `engine.params` and set `dirty = true`.
- Do not call `rebuild()` here. Re-planning happens at the top of the next `step()`, so it never runs concurrently with audio.
- For routing parameters: nothing to do; `step()` reads them from `v[]` each block.

### 4.6 `step(self, busFrames, numFramesBy4)`

```
numFrames = numFramesBy4 * 4
if dirty: engine.rebuild(); dirty = false; displayDirty = true
if !started and engine.params.loop: engine.trigger(); started = true; displayDirty = true
trig   = v[TriggerInput] ? busFrames + (v[TriggerInput]-1)*numFrames : null
reseed = v[ReseedInput]  ? busFrames + (v[ReseedInput]-1)*numFrames  : null
out    = busFrames + (v[Output]-1)*numFrames
replace = v[OutputMode]
for i in 0..numFrames-1:
    if reseed: rising edge (above 1.0 V after being below 0.5 V)
        -> newSeed = 1 + (hash(NT_getCpuCycleCount()) % 32767)
           NT_setParameterFromAudio(NT_algorithmIndex(self), kParamSeed + NT_parameterOffset(), newSeed)
           (the host then calls parameterChanged -> dirty -> applied on the next block)
    if trig: same edge detector -> engine.trigger(); started = true; displayDirty = true
    y = engine.process()
    out[i] = replace ? y : out[i] + y
if displayDirty: engine.renderCurve(display, 256); compute displayLo/Hi; displayDirty = false
```

- **Edge detection:** a Schmitt trigger. It goes high above 1.0 V and re-arms below 0.5 V. Only a low-to-high transition counts.
- **Triggers are sample-accurate:** handle them inside the frame loop, before `process()`.
- **Loop turned on while stopped:** if `engine.params.loop` is set and `!engine.running()`, call `engine.trigger()` at the top of the block.
- **Hash for the new seed:** any cheap 32-bit integer mix of the cycle counter, for example `x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15`.
- **Display refresh:** the engine's `renderCurve` is a single pass, which is cheap enough to run in `step()` only when something changed.

### 4.7 `draw(self)`

The screen is 256 × 64 pixels, 16 grey levels.

- Draw `display[]` as a polyline (`NT_drawShapeF(kNT_line, …)`) scaled between `displayLo` and `displayHi`, below the standard parameter line (y from about 12 to 63).
- Draw a vertical playhead at `x = 255 * engine.positionSec() / engine.durationSec()` while `engine.running()`.
- Optionally show the current output voltage with `NT_drawText` (format with `NT_floatToString`).
- Return `false` to keep the standard parameter line at the top.
- `draw()` only reads `display[]` and two engine getters. It must not call engine methods that change state.

## 5. Engine API (from section 9)

| Call | Use |
|---|---|
| `Engine()` | Constructs with the defaults in `Params` (the web prototype's defaults) |
| `setSampleRate(sr)` | Once, from `construct()` |
| `params` | Public struct: `lengthSec, windows, chaos, radiusSec, roundProb, holdProb, levelV, offsetV, bipolar, loop, seed` |
| `rebuild()` | Re-plan after changing `params`. Keeps the playback position |
| `reseed(n)` | Sets the seed and rebuilds |
| `trigger()` | Start or restart from the current level |
| `process()` | Next sample in volts; holds the last value when stopped |
| `running()`, `positionSec()`, `durationSec()` | For the display and loop logic |
| `renderCurve(out, n)` | Fills `n` evenly spaced samples of the current curve |
| `envelopeCount()`, `envelope(i)`, `breakpointCount()` | Inspection and debugging |

Cost:
- `process()` is a few multiplies per sample.
- `trigger()` builds at most a few hundred breakpoints with no random numbers.
- `rebuild()` draws about 700 random numbers and sorts two lists of 24.
- All three are fine inside `step()`, but `rebuild()` should run at most once per block.

## 6. What not to change

- The random key scheme, the region algorithm and the rounding formula. They are what make a seed reproducible between the web prototype and the module.
- "No reset" envelope behaviour and the carry-over of the level between passes.
- The clamp of each radius to half the neighbouring gaps.

## 7. Out of scope (ask before adding)

- CV modulation of Length, Chaos, Radius and the other dials. It is easy to add with extra CV input parameters, but not part of the current design.
- A clock input or tempo sync. The current design is free-running only.
- A multi-channel version.

## 8. Acceptance tests

### 8.1 Desktop test (before touching hardware)

Compile `contour_engine.h` on a desktop with the same flags (minus the ARM ones). Use a sample rate of 100 Hz: call `rebuild()`, then `trigger()`, then `process()` once per sample. Sample n is at time n × 0.01 s.

Each value below is the output at that time and must match within ±0.001 V.

**Case 1:** Length 6.0 s · Windows 6 · Chaos 60% · Radius 120 ms · Round prob 100% · Hold prob 0% · Level 8.0 V · Offset 0.0 V · Bipolar off · Seed 7 (all defaults)

| t (s) | 0 | 0.5 | 1 | 1.5 | 2 | 2.5 | 3 | 3.5 | 4 | 4.5 | 5 | 5.5 | 6 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| V | 5.3333 | 4.8929 | 5.6162 | 5.0606 | 7.2034 | 4.3570 | 3.8648 | 5.5147 | 5.3221 | 5.1143 | 5.4567 | 5.0208 | 4.1759 |

**Case 2:** Length 6.0 s · Windows 6 · Chaos 60% · Radius 220 ms · Round prob 60% · Hold prob 35% · Level 8.0 V · Offset 0.0 V · Bipolar off · Seed 7

| t (s) | 0 | 0.5 | 1 | 1.5 | 2 | 2.5 | 3 | 3.5 | 4 | 4.5 | 5 | 5.5 | 6 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| V | 5.3333 | 4.8929 | 5.6162 | 5.1717 | 7.2753 | 6.1624 | 3.8393 | 5.9604 | 5.5207 | 5.1865 | 5.4950 | 5.2327 | 4.1759 |

**Case 3:** Length 6.0 s · Windows 6 · Chaos 60% · Radius 220 ms · Round prob 60% · Hold prob 35% · Level 9.5 V · Offset +4.5 V · Bipolar off · Seed 7 (clips at +10 V)

| t (s) | 0 | 0.5 | 1 | 1.5 | 2 | 2.5 | 3 | 3.5 | 4 | 4.5 | 5 | 5.5 | 6 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| V | 10.0000 | 10.0000 | 10.0000 | 10.0000 | 10.0000 | 10.0000 | 9.0592 | 10.0000 | 10.0000 | 10.0000 | 10.0000 | 10.0000 | 9.4588 |

**Case 4:** Length 7.0 s · Windows 5 · Chaos 50% · Radius 250 ms · Round prob 50% · Hold prob 0% · Level 5.0 V · Offset 0.0 V · Bipolar on · Seed 9

| t (s) | 0 | 0.5 | 1 | 1.5 | 2 | 2.5 | 3 | 3.5 | 4 | 4.5 | 5 | 5.5 | 6 | 6.5 | 7 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| V | 0.3535 | 0.0237 | 0.4917 | 1.0917 | 0.0716 | -1.2661 | 0.1253 | 0.4924 | 1.3469 | 0.8309 | 0.9297 | 0.4802 | 0.5790 | 0.3968 | 0.3535 |

Also check:
- **Loop:** with Loop on, the engine runs for 25 s at 48 kHz without stopping, and the output stays within ±10 V.
- **Rebuild keeps position:** calling `rebuild()` mid-pass after a parameter change leaves `positionSec()` unchanged.

### 8.2 On the module

1. Add Contour with no inputs patched. Loop is on by default, so the output should run continuously and the screen should show the curve with a moving playhead.
2. Patch a clock to Trigger input and turn Loop off. Each rising edge should restart the function; between triggers the output should hold the last value.
3. Patch a gate to Reseed input. Each rising edge should change the Seed parameter and the curve.
4. Turn Radius from 0 to 500 ms. Corners inside the rounding regions should round off progressively with no delay in the output.
5. Turn Hold prob up. Flat held sections should appear, with jumps at their ends; those jumps should become S-curves where they fall in a rounding region.
6. Set Level 10.0 V and Offset +5.0 V. The output should clip flat at +10 V, with no wrap-around and no overshoot.
7. Watch the CPU load in the NT's CPU display while turning knobs quickly and triggering at audio rate. Usage should stay low; `rebuild()` runs at most once per block.

## 9. Reference engine: `contour_engine.h`

This is verified against the web prototype on five settings, including holds, rail clipping and bipolar output: maximum difference 0.0001 V. It builds warning-free with `-std=c++11 -fno-rtti -fno-exceptions -Wall -Wextra -pedantic` at `-O0` and `-Os`. It has not yet been compiled with the ARM toolchain.

```cpp
// contour_engine.h — Contour function generator engine (rounding-radius version), reference implementation.
// Self-contained: depends only on <math.h> and <stdint.h>. No heap, no exceptions, no RTTI, C++11.
// Verified sample-for-sample against the Contour Radius Generator web page (see CONTOUR_NT_SPEC.md).
//
//   contour::Engine eng;                 // Params defaults = page defaults
//   eng.setSampleRate(48000.0f);
//   eng.rebuild();                       // after changing eng.params (draws the random plan)
//   eng.trigger();                       // start or restart; the envelope level carries on
//   float volts = eng.process();         // once per sample, output in volts
#pragma once
#include <math.h>
#include <stdint.h>

namespace contour {

struct Params {
  float    lengthSec = 6.0f;   // 1..10 s
  int      windows   = 6;        // 1..32 equal windows, 2 envelopes in each
  float    chaos     = 0.6f;   // pulls each rate/level toward a random 0..99 target
  float    radiusSec = 0.12f;   // corner rounding: blends from radius before to radius after
  float    roundProb = 1.0f;   // share of the length where corners are rounded
  float    holdProb  = 0.0f;   // share of the length that is held
  float    levelV    = 8.0f;   // volts: 0..level, or +/-level when bipolar
  float    offsetV   = 0.0f;
  bool     bipolar   = false;
  bool     loop      = false;
  uint32_t seed      = 7u;
};

class Engine {
public:
  struct Env { double a, b; int win, sub, r1, l1, r2, l2; };   // start/end in seconds, rates, levels

  Params params;

  explicit Engine(const Params& p = Params()) : params(p) { rebuild(); }

  void  setSampleRate(float sr) { dt_ = 1.0 / sr; }
  void  reseed(uint32_t s)      { params.seed = s; rebuild(); }
  bool  running() const         { return running_; }
  float durationSec() const     { return (float)T_; }
  int   envelopeCount() const   { return nEnv_; }
  int   breakpointCount() const { return nPts_; }
  const Env& envelope(int i) const { return env_[i]; }
  double positionSec() const    { return t_; }
  // Fill out[0..n-1] with the current curve at n evenly spaced times from 0 to the length
  // (single pass; for a display). Does not change playback state. n >= 2.
  void renderCurve(float* out, int n) const {
    int k = 0;
    for (int i = 0; i < n; ++i) {
      const double t = T_ * i / (n - 1);
      while (k < nPts_ - 2 && t >= pt_[k + 1].t) ++k;
      out[i] = (float)eval(k, t);
    }
  }

  // Start or restart. The envelope level carries on from where it is, so the curve is re-planned
  // from that level (a few hundred simple operations, no random numbers).
  void trigger() {
    passStart_ = primed_ ? levelAt(t_) : (double)env_[nEnv_ - 1].l2;
    buildCurve(passStart_);
    primed_ = true; t_ = 0.0; k_ = 0; running_ = true;
  }

  // Draw the random plan from params (deterministic per seed): envelopes, rounded and held regions,
  // then rebuild the curve. Call after changing params.
  void rebuild() {
    const Params& p = params;
    T_ = clampd(p.lengthSec, 1.0, 10.0);
    const int W = imax(1, imin(p.windows, (int)kMaxWin));
    nEnv_ = 0;
    for (int wi = 0; wi < W; ++wi) {
      const double a = wi * T_ / W, b = (wi == W - 1) ? T_ : (wi + 1) * T_ / W, base = (b - a) / kEnvsPerWindow;
      double t = a;
      for (int s = 0; s < kEnvsPerWindow; ++s) {
        const double l = (s == kEnvsPerWindow - 1) ? b - t : base;
        const uint32_t idx = (uint32_t)nEnv_;
        // base rate is 0 and base level is random in 0..99; chaos pulls each toward its own random target
        auto pull = [&](double v, uint32_t j) { return (int)floor(v + p.chaos * (99.0 * rnd(3000 + idx, j) - v) + 0.5); };
        Env& e = env_[nEnv_++];
        e.a = t; e.b = t + l; e.win = wi; e.sub = s;
        e.r1 = pull(0.0, 1); e.l1 = pull(99.0 * rnd(2000 + idx, 2), 2);
        e.r2 = pull(0.0, 3); e.l2 = pull(99.0 * rnd(2000 + idx, 4), 4);
        t += l;
      }
    }
    nRound_ = probWindows(p.roundProb, 500000, round_);
    nHold_  = probWindows(p.holdProb,  600000, hold_);
    // Re-plan the current pass from the level it started at. Playback position and run state are
    // kept, so turning a parameter reshapes the curve without restarting it.
    buildCurve(passStart_ >= 0.0 ? passStart_ : (double)env_[nEnv_ - 1].l2);
    k_ = 0;
  }

  // One output sample in volts. Holds the last value when stopped.
  float process() {
    if (!running_) return out_;
    const double t = t_;
    while (k_ < nPts_ - 2 && t >= pt_[k_ + 1].t) ++k_;
    out_ = (float)eval(k_, t);
    t_ += dt_;
    if (t_ >= T_) { if (params.loop) trigger(); else running_ = false; }
    return out_;
  }

private:
  enum { kMaxWin = 32, kEnvsPerWindow = 2, kMaxEnv = kMaxWin * kEnvsPerWindow,
         kPieces = 24, kMaxLv = 3 * kMaxEnv + 2, kMaxPts = 3 * kMaxLv + 2 * kPieces + 8 };
  struct Span { double a, b; };
  struct Pt { double t, vl, vr, r, s1, s2; };   // time, value left/right of it, radius, slope in/out

  static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
  static double dmin(double a, double b) { return a < b ? a : b; }
  static double dmax(double a, double b) { return a > b ? a : b; }
  static int    imin(int a, int b) { return a < b ? a : b; }
  static int    imax(int a, int b) { return a > b ? a : b; }
  static double sweepSec(int rate) { return 20.0 * pow(2.0, -rate / 7.5); }   // full 0..99 move

  // mulberry32 keyed by (seed, i, j): bit-identical to the page's rnd(i, j)
  double rnd(uint32_t i, uint32_t j) const {
    uint32_t a = (uint32_t)((uint64_t)params.seed * 100003ull + (uint64_t)i * 7919ull + (uint64_t)j * 104729ull);
    a += 0x6D2B79F5u;
    uint32_t t = (a ^ (a >> 15)) * (1u | a);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;
  }

  // 24 random-length pieces visited in random order until prob * T is covered, then merged.
  int probWindows(float prob, uint32_t base, Span* out) const {
    if (prob >= 1.0f) { out[0] = {0.0, T_}; return 1; }
    if (prob <= 0.0f) return 0;
    double w[kPieces], a[kPieces], len[kPieces], sum = 0.0; int order[kPieces];
    for (int k = 0; k < kPieces; ++k) { w[k] = 0.25 + rnd(base + k, 1); sum += w[k]; order[k] = k; }
    double acc = 0.0;
    for (int k = 0; k < kPieces; ++k) { a[k] = acc; len[k] = w[k] / sum * T_; acc += len[k]; }
    double key[kPieces];
    for (int k = 0; k < kPieces; ++k) key[k] = rnd(base + k, 2);
    for (int i = 1; i < kPieces; ++i) {                          // insertion sort of order[] by key
      const int o = order[i]; int j = i - 1;
      while (j >= 0 && key[order[j]] > key[o]) { order[j + 1] = order[j]; --j; }
      order[j + 1] = o;
    }
    Span picked[kPieces]; int np = 0; double need = prob * T_;
    for (int oi = 0; oi < kPieces && need > 1e-9; ++oi) {
      const int k = order[oi];
      if (len[k] <= need) { picked[np++] = {a[k], a[k] + len[k]}; need -= len[k]; }
      else { const double off = rnd(base + k, 3) * (len[k] - need); picked[np++] = {a[k] + off, a[k] + off + need}; need = 0.0; }
    }
    for (int i = 1; i < np; ++i) {                               // insertion sort of picked[] by start
      const Span sp = picked[i]; int j = i - 1;
      while (j >= 0 && picked[j].a > sp.a) { picked[j + 1] = picked[j]; --j; }
      picked[j + 1] = sp;
    }
    int n = 0;
    for (int i = 0; i < np; ++i) {
      if (n && picked[i].a <= out[n - 1].b + 1e-9) out[n - 1].b = dmax(out[n - 1].b, picked[i].b);
      else out[n++] = picked[i];
    }
    return n;
  }

  // Envelope level (0..99) at time t of the current pass, before volts, holds and rounding.
  double levelAt(double t) const {
    int i = 0; while (i < nLv_ - 2 && t >= lvT_[i + 1]) ++i;
    const double u = clampd((t - lvT_[i]) / (lvT_[i + 1] - lvT_[i]), 0.0, 1.0);
    return lvV_[i] + (lvV_[i + 1] - lvV_[i]) * u;
  }
  double clippedAt(double t) const {
    int i = 0; while (i < nC_ - 2 && t >= cT_[i + 1]) ++i;
    const double u = clampd((t - cT_[i]) / (cT_[i + 1] - cT_[i]), 0.0, 1.0);
    return cV_[i] + (cV_[i + 1] - cV_[i]) * u;
  }

  // Turn the plan into breakpoints: levels -> volts with rail clipping -> holds -> corner radii.
  void buildCurve(double startLevel) {
    const Params& p = params;
    // 1. envelope levels as a continuous polyline: toward L1 at R1, then L2 at R2, then flat
    nLv_ = 0; double cur = startLevel;
    auto pushLv = [&](double t, double v) {
      if (nLv_ && t - lvT_[nLv_ - 1] < 1e-9) { lvV_[nLv_ - 1] = v; return; }
      if (nLv_ < kMaxLv) { lvT_[nLv_] = t; lvV_[nLv_++] = v; }
    };
    pushLv(0.0, cur);
    for (int i = 0; i < nEnv_; ++i) {
      const Env& e = env_[i]; double t = e.a; pushLv(t, cur);
      for (int st = 0; st < 2 && t < e.b - 1e-12; ++st) {
        const int tgt = st ? e.l2 : e.l1;
        const double sp = 99.0 / sweepSec(st ? e.r2 : e.r1), need = fabs(tgt - cur) / sp;
        if (t + need <= e.b) { t += need; cur = tgt; }
        else { cur += (tgt > cur ? 1.0 : -1.0) * sp * (e.b - t); t = e.b; }
        pushLv(t, cur);
      }
    }
    pushLv(T_, cur);
    // 2. volts, clipped at the +/-10 V rails (a point is added where a segment crosses a rail)
    auto toV = [&](double L) { const double y = L / 99.0; return (p.bipolar ? p.levelV * (2.0 * y - 1.0) : p.levelV * y) + p.offsetV; };
    nC_ = 0;
    auto pushC = [&](double t, double v) { if (nC_ < kMaxPts) { cT_[nC_] = t; cV_[nC_++] = v; } };
    const double rails[2] = {-10.0, 10.0};
    for (int i = 0; i < nLv_; ++i) {
      const double v0 = toV(lvV_[i]);
      pushC(lvT_[i], clampd(v0, -10.0, 10.0));
      if (i + 1 < nLv_) {
        const double v1 = toV(lvV_[i + 1]); double u[2], rv[2]; int nc = 0;
        for (int q = 0; q < 2; ++q) if ((v0 - rails[q]) * (v1 - rails[q]) < 0.0) { u[nc] = (rails[q] - v0) / (v1 - v0); rv[nc++] = rails[q]; }
        if (nc == 2 && u[0] > u[1]) { double x = u[0]; u[0] = u[1]; u[1] = x; x = rv[0]; rv[0] = rv[1]; rv[1] = x; }
        for (int c = 0; c < nc; ++c) pushC(lvT_[i] + u[c] * (lvT_[i + 1] - lvT_[i]), rv[c]);
      }
    }
    // 3. holds: freeze at the value on entry, jump back on exit
    nPts_ = 0;
    auto pushP = [&](double t, double vl, double vr) { if (nPts_ < kMaxPts) pt_[nPts_++] = {t, vl, vr, 0.0, 0.0, 0.0}; };
    int ci = 0;
    for (int h = 0; h < nHold_; ++h) {
      const double a = hold_[h].a, b = hold_[h].b;
      while (ci < nC_ && cT_[ci] < a - 1e-9) { pushP(cT_[ci], cV_[ci], cV_[ci]); ++ci; }
      const double va = clippedAt(a), vb = clippedAt(b);
      pushP(a, va, va);
      pushP(b, va, b >= T_ - 1e-9 ? va : vb);
      while (ci < nC_ && cT_[ci] <= b + 1e-9) ++ci;
    }
    while (ci < nC_) { pushP(cT_[ci], cV_[ci], cV_[ci]); ++ci; }
    // 4. radius per breakpoint: the dial value inside a rounding window, never more than half
    //    the distance to a neighbour, so rounded corners cannot overlap
    for (int i = 1; i < nPts_; ++i) {
      Pt& q = pt_[i]; const double d1 = q.t - pt_[i - 1].t;
      q.s1 = (q.vl - pt_[i - 1].vr) / d1;
      if (i == nPts_ - 1) break;
      const double d2 = pt_[i + 1].t - q.t;
      q.s2 = (pt_[i + 1].vl - q.vr) / d2;
      bool in = false;
      for (int w = 0; w < nRound_; ++w) if (q.t >= round_[w].a && q.t <= round_[w].b) in = true;
      q.r = (in && p.radiusSec > 0.0f) ? dmin((double)p.radiusSec, dmin(d1 * 0.5, d2 * 0.5)) : 0.0;
    }
  }

  // Value on segment k. A rounded corner is a parabola tangent to both segments; a jump (the end
  // of a hold) becomes an S-curve across the same span.
  double eval(int k, double t) const {
    const Pt& A = pt_[k]; const Pt& B = pt_[k + 1];
    const double lin = A.vr + B.s1 * (t - A.t);
    if (B.r > 0.0 && t > B.t - B.r) {
      const double u = (t - (B.t - B.r)) / (2.0 * B.r);
      return lin + (B.s2 - B.s1) * B.r * u * u + (B.vr - B.vl) * u * u * (3.0 - 2.0 * u);
    }
    if (A.r > 0.0 && t < A.t + A.r) {
      const double u = (t - (A.t - A.r)) / (2.0 * A.r);
      return A.vl + A.s1 * (t - A.t) + (A.s2 - A.s1) * A.r * u * u + (A.vr - A.vl) * u * u * (3.0 - 2.0 * u);
    }
    return lin;
  }

  Env    env_[kMaxEnv];
  Span   round_[kPieces], hold_[kPieces];
  Pt     pt_[kMaxPts];
  double lvT_[kMaxLv], lvV_[kMaxLv], cT_[kMaxPts], cV_[kMaxPts];
  int    nEnv_ = 0, nRound_ = 0, nHold_ = 0, nLv_ = 0, nC_ = 0, nPts_ = 0, k_ = 0;
  double T_ = 1.0, dt_ = 1.0 / 48000.0, t_ = 0.0, passStart_ = -1.0;
  bool   running_ = false, primed_ = false;
  float  out_ = 0.0f;
};

} // namespace contour
```
