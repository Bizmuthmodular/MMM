# Period Stretch — real-time, period-synchronous time stretch for the 4x4 plugin

## Context

The 4x4 Disting NT plugin has two DRAM audio buffers:

- **Render buffer** — a new sound is rendered into it, amortized across `step()` calls.
- **Play buffer** — the last completed render is played back from it.

When a render finishes, the buffers swap roles.

This feature adds a time stretch to **playback only**. The audio in the play buffer is never modified or copied. Stretching is purely a matter of how the read head moves through the buffer. The render path is unaffected, except that it also builds a small analysis map (see below).

Adapt everything here to the existing 4x4 code structure. The names below are suggestions, not requirements.

## Concept

The stretcher looks for **periodic repetitions of a chosen length** in the audio. When it finds them, it extends them by replaying whole periods, with a crossfade at each splice. Material that isn't periodic at that length plays through at 1×.

One parameter, **Scale**, selects which repetition length to target. It spans a continuum:

| Scale | Period range | What gets stretched | Character |
|---|---|---|---|
| Short | ~2–20 ms | pitch cycles | PSOLA/WSOLA-like; pitch is preserved, smooth stretch of tonal material |
| Mid | ~20–200 ms | grains, note-level repetition | textural, stuttery |
| Long | ~200 ms–2 s | rhythmic loops, bars | "smart beat-repeat": extends parts of the pattern that already repeat |

Because the buffer is static while it plays, every stretch decision is made at read time. All parameters can therefore be changed in real time, and they take effect at the next splice point.

## Architecture

```
render step() ──► render buffer ──► period-map builder (amortized, per scale)
                                         │
                       swap (buffer + its maps together)
                                         │
play buffer + maps ──► read head ──► splice/crossfade logic ──► output
                          ▲
             Scale / Stretch / Threshold / Xfade params
```

Each buffer owns its own set of period maps. When the buffers swap, swap the map pointers along with the audio pointers.

## Period maps (built during render)

### Scales

Use octave-spaced target lengths, each searched over the lag range `[L/√2, L·√2]`. For example, with 11 scales: L = 2, 4, 8, …, 2048 ms. Make the scale set a compile-time constant.

### Map entry (per scale, per hop)

```cpp
struct PeriodEntry {
    uint32_t period;     // detected period in samples (full rate)
    uint8_t  confidence; // 0..255 = normalized autocorrelation peak, clamped to 0..1
};
```

- Hop: `max(L/2, 256 samples)`.
- Memory: a 10 s buffer at 48 kHz is ~1.9k entries per scale at the minimum hop, so tens of KB for all scales. Allocate two sets (one per buffer) in DRAM via `calculateRequirements`.

### Detection

For each hop position, compute the normalized autocorrelation

```
r(τ) = Σ x[n]·x[n+τ] / sqrt(Σ x[n]² · Σ x[n+τ]²)
```

over a window of about `2·L·√2`, for τ in the scale's lag range. Then:

- `period` = argmax τ
- `confidence` = max(0, r(τ))

### Keeping it cheap

The search range should be roughly 64–128 lag candidates regardless of scale:

- **Decimate** by `D ≈ L / 64` for analysis. A simple box/one-pole lowpass then downsample is fine, since this is analysis only.
- **Long scales (≳100 ms):** autocorrelate an RMS or rectified envelope rather than the waveform. Bar-level periodicity lives in the envelope.
- **Short scales:** after the coarse decimated search, refine the peak at full rate within ±D samples, so splices land on accurate pitch periods.
- **Amortize:** analyse each block as it is rendered, or in a post-render pass spread over `step()` calls. Either way, the maps must be complete before the swap.
- **Optional:** if the scale set or analysis budget is a problem, mark a buffer "analysed up to sample N". Until analysis catches up, playback past N runs at 1×.

## Playback algorithm

### Read-head state

```cpp
struct StretchState {
    uint32_t pos;          // read position in play buffer
    uint32_t segStart;     // start of current period segment
    uint32_t period;       // current period length
    float    debt;         // accumulated "extra periods owed" (stretch accumulator)
    // crossfade
    bool     fading;
    uint32_t fadePosOld;   // position of outgoing read head
    uint32_t fadeLen, fadeIdx;
};
```

### At each period boundary (pos reaches segStart + period)

1. Look up the map entries for the current position at the active scale(s). See "Scale knob" for how to choose between scales.
2. Add to the stretch accumulator: `debt += (stretch − 1) · w(conf)`, where `w(conf)` is 0 below **Threshold** and rises to 1 above it. A soft ramp avoids chattering.
3. If `debt ≥ 1`, **repeat**: jump the read head back by `period` and start a crossfade from the continuing position to the jumped-back position. Then `debt −= 1`.
4. Otherwise **continue**: the next segment starts at `pos`, using the period from the map.
5. Optionally, do a WSOLA-style refinement on the jump target: search ±a few ms for the best correlation with the outgoing audio. This matters mainly for short scales.

### Crossfade

- Length: `min(period / 2, Xfade param)`.
- Use a linear crossfade. The spliced segments are highly correlated by construction, so linear keeps level constant. Equal-power would bump the level.

### Freeze

When Stretch is at maximum, treat it as **freeze**: always repeat the current period while confidence is above the threshold. This is cheap and musically useful.

### Compression (optional)

Stretch < 1: when `debt ≤ −1` and confidence is high, skip forward one period instead of repeating. Implement this later if wanted.

### Buffer ends

- Never jump back before sample 0.
- At the end of the buffer, see "Swap handling" below.

### Multichannel

Analyse a mono sum. Apply identical splice decisions to all channels so the stereo image stays coherent.

## Parameters

| Param | Range | Notes |
|---|---|---|
| Scale | continuous knob across the octave scale set | see blending below |
| Stretch | 1× … 8×, top = freeze | read-time only |
| Threshold | 0 … 1 | minimum periodicity confidence before stretching |
| Xfade | ~1–50 ms | capped at period/2 |
| Swap mode | Immediate / Period boundary / After playback | see below |

All parameters are read-time only, so they are safe to change continuously.

### Scale knob

The knob's position falls between two adjacent scales `k` and `k+1` with fraction `f`. Score each scale as

```
score_k   = conf_k   · (1 − f)
score_k+1 = conf_k+1 · f
```

and take the period from the higher-scoring scale.

**Do not average the two periods.** An averaged period corresponds to neither scale and won't splice cleanly. This weighting makes the knob feel continuous while snapping to real periodicities.

## Swap handling

The stretch makes playback take longer than the sound's nominal length, which interacts with when buffers swap:

- **After playback:** swap when the read head reaches the end. The stretch naturally delays the swap, and the renderer gets more time. This is the simplest option.
- **Period boundary:** a render completes mid-playback, and the swap waits for the next splice point. It then crossfades from the old buffer's position into the new buffer's start. The old buffer must stay valid for the fade duration, so the renderer must not start writing into it until the fade finishes.
- **Immediate:** the same as Period boundary, but the crossfade starts at once.

At the swap, reset `debt` and the read-head state, and switch to the new buffer's maps.

**Open question:** confirm how 4x4 currently triggers the swap (render-complete vs playback-complete) and keep that as the default mode.

## CPU / memory summary

- **Playback:** per sample, at most two buffer reads plus a crossfade multiply. Per period, one map lookup plus an optional small WSOLA refinement. This is near the cost of plain playback.
- **Render-side analysis:** roughly (number of scales) × (~100 lags) × (window) per hop, at decimated rates. Profile this on the device, and reduce the scale count or lag candidates if render time suffers.
- **Memory:** two sets of period maps, tens of KB in total. No extra audio buffers.

## Suggested implementation order

1. **Desktop test harness first.** Port the analysis and read-head logic into a small standalone C++ program that processes WAV files on the Mac. Iterate on sound and correctness there before touching the NT.
2. Implement a single-scale map plus repeat-with-crossfade playback at a fixed Stretch.
3. Add the multi-scale maps and the Scale knob blending.
4. Add the Threshold soft ramp, Freeze, and the swap modes.
5. Port to the plugin: amortize the analysis in the render path, add DRAM allocation for the maps, and swap maps together with the buffers.
6. Profile render-time analysis cost on the NT, and tune the scale count and decimation.
7. Optional: compression (Stretch < 1) and WSOLA refinement at splices.
