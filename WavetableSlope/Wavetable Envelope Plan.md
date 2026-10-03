# Wavetable Envelope Plan — WavetableSlope

Full design reference for implementing `step()` (and the small remaining piece of `parameterChanged()`) in `WavetableSlope.cpp`. Everything else in the `.cpp` (parameter/page tables, memory allocation, wavetable-selection wiring, SD-card-mount handling) is already written and compiles — this document is what's needed to finish the DSP.

## What it does

`output(t) = ampEnvelope(t) × wavetableWindowSample(tablePhase(t))`

A standard two-segment **linear** Rise/Fall envelope amplitude-modulates a slow, interpolated scan through a small window of raw sample data pulled from an SD-card wavetable. The wavetable snippet is not played as an audio-rate oscillator — it's stretched out to provide the fine-grained *shape*/texture of the envelope itself, while `RiseTime`/`FallTime` fix the overall wall-clock duration of each segment.

- **ampEnvelope(t)**: 0→1 linearly over `RiseTime` seconds, then 1→0 linearly over `FallTime` seconds. Fixed wall-clock timing — **not** affected by `Stretch`. No sustain/hold/curve.
- **wavetableWindowSample**: fractional-index, linearly-interpolated read (same technique as `BKICK.cpp`'s transient-buffer read, ~line 605-619) into a `Window`-sample-long (2–1000) slice of the *entire* loaded wavetable — all waves concatenated into one flat buffer, wave boundaries ignored. Slice starts at `Offset`% of the way across `(totalSamples − Window)`; 0% = the wavetable's first sample.
- **Read rate**: nominal rate per segment = `Window / (segmentSeconds × sampleRate)` table-samples/output-sample, so at `Stretch = 100%` exactly one full pass across the window happens within that segment's duration. `Stretch` (10%–1000%) multiplies only this read rate — never the envelope timing. Phase wraps modulo `Window` (never a hard stop); the read is always interpolated regardless of rate ("keeping the slope resolution").
- **Fall segment**: restarts the table read from the window's first sample again — an independent second forward pass, not a reversal, not a continuation from where Rise ended.
- **Triggering**: `TriggerIn` CV input, polled every sample; rising edge crossing 1.0V (same convention as `DualPhaser.cpp`'s exciter) restarts Rise and always runs the full Rise+Fall to completion once fired (one-shot AD, no gate-following).
- **Snapshot-at-trigger**: at the trigger instant, `Window`/`Offset`/`RiseTime`/`FallTime`/`Stretch` and the wavetable's base pointer/size are converted to sample-domain values and frozen into the DTC for the life of that envelope (matches `BKICK.cpp`'s transient-buffer snapshot pattern) — twiddling knobs mid-envelope never glitches the currently-playing shot.
- **Output**: raw `int16_t` sample (±32768) → normalize to [-1,1) → × envelope (0..1) → × 10.0f → hard-clamp ±10V → write to `SlopeOut` in Add/Replace mode.

## Struct layout (already in the .cpp)

```cpp
enum EnvState : uint8_t { kEnvIdle = 0, kEnvRise = 1, kEnvFall = 2 };

struct WavetableSlopeAlg_DTC {
    EnvState envState;
    uint32_t envSampleCount;
    float    tablePhase;
    float    prevTrigIn;
    // Snapshot-at-trigger (frozen for the life of the current envelope):
    const int16_t* tableBaseSnap;
    uint32_t windowStartSnap, windowSizeSnap;
    uint32_t riseSamplesSnap, fallSamplesSnap;
    float    riseRateBaseSnap, fallRateBaseSnap, stretchFactorSnap;
};
```

Types are deliberately wider than the `int16_t` parameters: `RiseTime`=10.00s at 48kHz is 480,000 samples, and `windowStartSnap` ranges across the whole loaded table (potentially 500K+ samples).

`tableBaseSnap` is `nullptr` until the first successful trigger (DTC is `memset` to 0 in `construct()`) — the `kEnvIdle` state must never dereference it.

## Total sample count / base pointer (compute at trigger time, from `request`)

```cpp
uint32_t totalSamples = a->request.numWaves * a->request.waveLength;
const int16_t* tableBase = a->request.table +
    ( a->request.usingMipMaps ? (a->request.waveLength * a->request.numWaves) : 0 );
```
Skips the mip-map pyramid if present (formula confirmed against `wav.h`'s layout comment block, ~line 145-162) — operates only on the full-resolution concatenated data.

## step() — full near-final code to transcribe

```cpp
void step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
    WavetableSlopeAlg*     a = (WavetableSlopeAlg*)self;
    WavetableSlopeAlg_DTC* d = a->dtc;
    int numFrames = numFramesBy4 * 4;

    // --- SD card mount transition: patch Wavetable.max, kick off (re)load ---
    bool cardMounted = NT_isSdCardMounted();
    if ( a->cardMounted != cardMounted )
    {
        a->cardMounted = cardMounted;
        if ( cardMounted )
        {
            a->params[kParamWavetable].max = NT_getNumWavetables() - 1;
            NT_updateParameterDefinition( NT_algorithmIndex(self), kParamWavetable );
            parameterChanged( self, kParamWavetable );
        }
    }

    // --- Bus resolution ---
    int triggerBus = a->v[kParamTriggerIn];
    const float* trig = (triggerBus > 0) ? busFrames + (triggerBus - 1) * numFrames : nullptr;

    float* out     = busFrames + (a->v[kParamSlopeOut] - 1) * numFrames;   // min=1, always valid
    bool   replace = (a->v[kParamSlopeOutMode] != 0);

    // --- Locals from DTC ---
    EnvState envState        = d->envState;
    uint32_t envSampleCount  = d->envSampleCount;
    float    tablePhase      = d->tablePhase;
    float    prevTrig        = d->prevTrigIn;
    const int16_t* tableBaseSnap = d->tableBaseSnap;
    uint32_t windowStartSnap = d->windowStartSnap;
    uint32_t windowSizeSnap  = d->windowSizeSnap;
    uint32_t riseSamplesSnap = d->riseSamplesSnap;
    uint32_t fallSamplesSnap = d->fallSamplesSnap;
    float    riseRateBaseSnap  = d->riseRateBaseSnap;
    float    fallRateBaseSnap  = d->fallRateBaseSnap;
    float    stretchFactorSnap = d->stretchFactorSnap;

    for ( int i = 0; i < numFrames; ++i )
    {
        // --- Trigger detection (only if TriggerIn bus is connected) ---
        if ( trig )
        {
            float t = trig[i];
            if ( t >= 1.0f && prevTrig < 1.0f )
            {
                if ( a->tableReady && a->request.numWaves > 0 && a->request.waveLength > 0 )
                {
                    uint32_t totalSamples = a->request.numWaves * a->request.waveLength;
                    if ( totalSamples > 0 )
                    {
                        const int16_t* tableBase = a->request.table +
                            ( a->request.usingMipMaps ? (a->request.waveLength * a->request.numWaves) : 0 );

                        // Window: clamp to totalSamples
                        uint32_t winParam = (uint32_t)a->v[kParamWindow];
                        uint32_t winSize  = (winParam < totalSamples) ? winParam : totalSamples;
                        if ( winSize < 1 ) winSize = 1;

                        // Offset: tenths-of-percent -> fraction -> start index
                        float    offsetFrac = (float)a->v[kParamOffset] * 0.001f;   // 0..1
                        uint32_t span       = totalSamples - winSize;
                        uint32_t winStart   = (uint32_t)( offsetFrac * (float)span + 0.5f );
                        if ( winStart > span ) winStart = span;

                        // Rise/Fall: hundredths-of-second -> seconds -> samples
                        float sr      = NT_globals.sampleRate;
                        float riseSec = (float)a->v[kParamRiseTime] * 0.01f;
                        float fallSec = (float)a->v[kParamFallTime] * 0.01f;
                        uint32_t riseS = (uint32_t)( riseSec * sr + 0.5f ); if ( riseS < 1 ) riseS = 1;
                        uint32_t fallS = (uint32_t)( fallSec * sr + 0.5f ); if ( fallS < 1 ) fallS = 1;

                        float stretch = (float)a->v[kParamStretch] * 0.01f;   // 0.1x .. 10x

                        tableBaseSnap      = tableBase;
                        windowStartSnap    = winStart;
                        windowSizeSnap     = winSize;
                        riseSamplesSnap    = riseS;
                        fallSamplesSnap    = fallS;
                        riseRateBaseSnap   = (float)winSize / (float)riseS;
                        fallRateBaseSnap   = (float)winSize / (float)fallS;
                        stretchFactorSnap  = stretch;

                        envState       = kEnvRise;
                        envSampleCount = 0;
                        tablePhase     = 0.0f;
                    }
                }
            }
            prevTrig = t;
        }

        // --- Envelope + interpolated window read ---
        float ampEnv = 0.0f, sampleNorm = 0.0f;

        if ( envState == kEnvRise )
        {
            ampEnv = (float)envSampleCount / (float)riseSamplesSnap;

            int   i0   = (int)tablePhase;
            float frac = tablePhase - (float)i0;
            int   i1   = ( i0 + 1 < (int)windowSizeSnap ) ? i0 + 1 : 0;
            int16_t s0 = tableBaseSnap[ windowStartSnap + i0 ];
            int16_t s1 = tableBaseSnap[ windowStartSnap + i1 ];
            sampleNorm = ( (float)s0 * (1.0f - frac) + (float)s1 * frac ) * (1.0f / 32768.0f);

            tablePhase += riseRateBaseSnap * stretchFactorSnap;
            while ( tablePhase >= (float)windowSizeSnap ) tablePhase -= (float)windowSizeSnap;

            if ( ++envSampleCount >= riseSamplesSnap )
            {
                envSampleCount = 0;
                envState   = kEnvFall;
                tablePhase = 0.0f;   // independent forward pass, NOT a continuation of Rise
            }
        }
        else if ( envState == kEnvFall )
        {
            ampEnv = 1.0f - (float)envSampleCount / (float)fallSamplesSnap;

            int   i0   = (int)tablePhase;
            float frac = tablePhase - (float)i0;
            int   i1   = ( i0 + 1 < (int)windowSizeSnap ) ? i0 + 1 : 0;
            int16_t s0 = tableBaseSnap[ windowStartSnap + i0 ];
            int16_t s1 = tableBaseSnap[ windowStartSnap + i1 ];
            sampleNorm = ( (float)s0 * (1.0f - frac) + (float)s1 * frac ) * (1.0f / 32768.0f);

            tablePhase += fallRateBaseSnap * stretchFactorSnap;
            while ( tablePhase >= (float)windowSizeSnap ) tablePhase -= (float)windowSizeSnap;

            if ( ++envSampleCount >= fallSamplesSnap )
            {
                envSampleCount = 0;
                envState = kEnvIdle;
            }
        }
        // kEnvIdle: ampEnv=0, sampleNorm=0 — deliberately skip tableBaseSnap access entirely
        // (tableBaseSnap may still be nullptr before the first-ever trigger)

        float v = ampEnv * sampleNorm * 10.0f;
        if      ( v >  10.0f ) v =  10.0f;
        else if ( v < -10.0f ) v = -10.0f;
        out[i] = replace ? v : out[i] + v;
    }

    // --- Write state back to DTC ---
    d->envState = envState;  d->envSampleCount = envSampleCount;  d->tablePhase = tablePhase;
    d->prevTrigIn = prevTrig;
    d->tableBaseSnap = tableBaseSnap;
    d->windowStartSnap = windowStartSnap;  d->windowSizeSnap = windowSizeSnap;
    d->riseSamplesSnap = riseSamplesSnap;  d->fallSamplesSnap = fallSamplesSnap;
    d->riseRateBaseSnap = riseRateBaseSnap;  d->fallRateBaseSnap = fallRateBaseSnap;
    d->stretchFactorSnap = stretchFactorSnap;
}
```

Key formulas, called out explicitly:
- s→samples: `samples = (uint32_t)(seconds * NT_globals.sampleRate + 0.5f)`, clamped `>= 1`.
- `RiseTime`/`FallTime` raw→seconds: `v * 0.01f` (matches `kNT_scaling100`).
- `Offset` raw→fraction: `v * 0.001f` (matches `kNT_scaling10`, tenths-of-percent raw → 0..1).
- `Stretch` raw→multiplier: `v * 0.01f` (raw 10..1000 → 0.1x..10x — no extra `kNT_scaling` divisor since the parameter is already stored as a raw percent).
- Nominal rate: `windowSize / riseSamples` or `windowSize / fallSamples` (table-samples per output-sample at Stretch=100%).
- Actual per-sample increment: `nominalRate * stretchFactor`.
- `windowStart = round(offsetFrac * (totalSamples - windowSize))`.
- Wrap: `while (tablePhase >= windowSize) tablePhase -= windowSize;` — cheap; worst-case wraps-per-sample is `Stretch_max/riseSamples_min ≈ 10/480 ≈ 0.02`, so this essentially never loops more than once regardless of parameter combination.
- No `sinf`/`cosf`/`expf` in the hot loop — purely integer/float arithmetic, bounded work per sample, compliant with the hard-real-time constraint on `step()`.

## `parameterChanged()` — already fully implemented in the .cpp

Only `kParamWavetable` needs handling — kicks off `NT_readWavetable()` if not already awaiting a callback. None of `Window`/`Offset`/`RiseTime`/`FallTime`/`Stretch` need caching in `parameterChanged()`: they're only ever converted to sample-domain values at the instant of a trigger (the snapshot-at-trigger block above), so caching them earlier would be redundant.

## Edge cases already handled by the design above

1. **SD card unmounted / never mounted** → `tableReady` stays `false`; `step()` (already implemented for this part) polls `NT_isSdCardMounted()` and only patches `Wavetable.max` + kicks off the first load on the false→true transition.
2. **Wavetable never loaded, or `request.error==true`** → guarded by `a->tableReady`, reset to `false` at the start of every new load attempt in `parameterChanged()`.
3. **`Window > totalSamples`** (short file) → clamped at trigger-snapshot time via `winSize = min(winParam, totalSamples)`, floor 1.
4. **`TriggerIn` bus == 0 (disconnected)** → `trig` pointer is `nullptr`, trigger-detection block skipped entirely per-sample, never indexes `busFrames` at -1.
5. **Re-triggering while an envelope is already running** → unconditionally resets `envState=kEnvRise`, `envSampleCount=0`, `tablePhase=0`, re-snapshots all derived quantities from the then-current parameter values — matches `BKICK.cpp`'s unconditional trigger-reset pattern.
6. **Reading `tableBaseSnap` before any trigger has ever fired** → `nullptr` after `construct()` (DTC `memset` to 0). Only `kEnvRise`/`kEnvFall` dereference it, and those states are only entered via the trigger branch that has already set a valid `tableBaseSnap`.
7. **New wavetable selected while an envelope from the previous file is still playing** → reads stay memory-safe (bounded by the fixed buffer since old `windowStart+windowSize <= old totalSamples <= request.tableSize`), but content read during the reload could be transient/stale data from the new file streaming in — a possible momentary audible glitch, not a crash. Not specially guarded against; acceptable, same risk profile as the SDK examples this design is based on.
8. **Degenerate 1-sample table** (`totalSamples==1`) → `winSize` clamps to 1, `windowStartSnap=0`; `i0`/`i1` wrap logic collapses to reading the same sample twice, producing a flat but non-crashing output.
9. **`request.tableSize` too small for a real file with mip-maps** → surfaces as `request.error`, caught by guard #2. `kWavetableBufferFrames` (~2MB, in the .cpp) is a best guess pending real `/wavetables/` file testing on hardware — bump it if `request.error` fires on a legitimate file.

## Verification once step() is filled in

1. Recompile: `make cpp SOURCE=WavetableSlope/WavetableSlope.cpp OUT=WavetableSlope/WavetableSlope.o` from the project root — zero warnings under `-Wall`.
2. Manual desk-check against this doc: bounds on `i0`/`i1`, the `windowStart+windowSize <= totalSamples` invariant, the `TriggerIn==0` guard.
3. Load on real hardware with an actual `/wavetables/` file and confirm `request.error` never fires (validates the `kWavetableBufferFrames` sizing) and that triggering produces the expected shaped rise/fall CV on a scope or LED.
