# Task: add a "Noise Mask" stage to my Disting NT plugin

I want a bipolar crossfader-style control that, in real time, replaces the **noisy** parts of a rendered sound with its **clean** parts (turned left) or the **clean** parts with **noisy** parts (turned right). The centre does nothing. The sound is already rendered into a RAM buffer by the plugin, so analysis is done offline per render and playback stays cheap.

This is likely going into my **4x4** plugin (double-buffered DRAM renderer/player: one buffer renders while the other plays). If that's the case, the analysis must belong to a buffer and swap together with it — the playing buffer always uses the analysis of *its own* content.

## Before writing code

1. Read the existing plugin source and the distingNT_API headers in this repo. Use only APIs that actually exist there — do not invent functions, and do not assume CMSIS-DSP or an FFT library is available (none is needed for this feature).
2. Identify: where the render finishes, how buffers are allocated (DRAM vs fast memory), how the double-buffer swap happens, and how `step()` processes blocks.
3. Summarise the integration points back to me and propose the parameter list before implementing. Ask if anything is ambiguous.

## Architecture

Put the DSP in its own header/source pair (e.g. `noisemask.h/.cpp`), independent of the NT API so it can be unit-tested on desktop. The plugin glue only feeds it buffers, parameters and output.

### 1. Analysis (once per completed render, spread across `step()` calls)

Never analyse the whole buffer in one call — process a few frames per `step()` so the audio deadline is never at risk. Expose progress so the display can show it, and only treat the analysis as valid once complete (until then: pass-through).

Frame the buffer (start with 1024 samples, hop 512). For each frame, copy it to a small scratch buffer in fast memory and Hann-window it, then compute:

- **Noisiness score** — LPC prediction-error ratio via autocorrelation + Levinson-Durbin, order 12–16. Use the final prediction error directly; do not filter the audio.
  ```cpp
  // x: windowed frame. Returns 0 (clean/predictable) .. 1 (white noise)
  float frameNoisiness(const float* x, int N, int order /* <= 16 */)
  {
      float r[17];
      for (int k = 0; k <= order; ++k) {
          float acc = 0.f;
          for (int n = k; n < N; ++n) acc += x[n] * x[n - k];
          r[k] = acc;
      }
      if (r[0] < 1e-9f) return 0.f;
      r[0] *= 1.0001f;                 // noise floor keeps the recursion stable
      float a[17] = {0}, tmp[17];
      float err = r[0];
      for (int i = 1; i <= order; ++i) {
          float acc = r[i];
          for (int j = 1; j < i; ++j) acc -= a[j] * r[i - j];
          float k = acc / err;
          for (int j = 1; j < i; ++j) tmp[j] = a[j] - k * a[i - j];
          for (int j = 1; j < i; ++j) a[j] = tmp[j];
          a[i] = k;
          err *= (1.f - k * k);
      }
      return err / r[0];
  }
  ```
  Store it in dB (`10*log10(ratio)`) — ranking is unaffected but it is easier to inspect.
- **Period estimate** — normalised autocorrelation peak over the pitch-lag range (e.g. 40 Hz – 2 kHz at the current sample rate), on the unwindowed frame. Store lag in samples (0 = no clear period).
- **RMS** of the frame.
- Silent frames (RMS below a floor): flag them; they are never masked and never used as fill source.

After all frames:
- Median-filter the scores over 5 frames.
- Compute `rank[f]` (0 = cleanest … F-1 = noisiest) by sorting frame indices by smoothed score. Sorting a few thousand floats once per render is fine; spread it out if profiling says otherwise.

### 2. Control mapping (per frame boundary during playback)

Control `x` in −1..+1 (parameter −100..+100 %). Rank-based so it behaves the same on any material, and masked sets are nested so no region rebuild is ever needed:

```cpp
bool frameMasked(int f, float x)
{
    const float dead = 0.03f;      // centre dead zone = pure pass-through
    const float maxFrac = 0.9f;    // always leave source material
    float a = (fabsf(x) - dead) / (1.f - dead);
    if (a <= 0.f) return false;
    int n = (int)(a * maxFrac * numFrames);
    return (x < 0) ? rank[f] >= numFrames - n   // left: noisiest n replaced by clean
                   : rank[f] <  n;              // right: cleanest n replaced by noisy
}
```

- Smooth the parameter (one-pole) and quantise `n` to ~128 steps to avoid boundary flicker.
- Mask decisions change **only at frame boundaries**.
- **Minimum run length**: when entering a masked frame, look ahead through the buffer; if the masked run is shorter than the *Min run* parameter (default 40 ms), leave it unmasked.

### 3. Playback (per sample)

Output timeline is unchanged — the playhead runs through the buffer normally:

```
out = (1 - w) * original[pos] + w * fill.next()
```

- `w` is 0 in kept frames and 1 in masked runs, ramping over the *Boundary fade* parameter (default 10 ms) **inside** the masked run, so kept audio is never touched. Equal-power ramp at boundaries (original and fill are uncorrelated).
- **Fill source = the most recent kept run the playhead passed through.** Track its start/end while playing. If no kept run has been played yet (masked run at buffer start, or after a loop wrap), look ahead for the next kept run.
- **Fill generator**: two (or three) grain voices, ping-pong, reading from the source run.
  - **Left side (clean fill)**: loop length snapped to an integer multiple of the source's period estimate (fallback: fixed ~50 ms if no period), loop start aligned at the source start, crossfade length 2–4 periods, **linear (equal-gain)** crossfade — the signals are correlated.
  - **Right side (noisy fill)**: grains of ~100–200 ms read from **random offsets** within the source (and optionally across all kept noisy runs, for variety), overlap 10–30 ms, **equal-power** crossfade — the signals are uncorrelated. Never loop a short noise segment at a fixed offset (it becomes pitched).
- **Level match**: scale the fill by `RMS(frames being replaced) / RMS(source run)`, interpolated per frame so the masked section keeps the original's dynamics. Clamp the gain (e.g. ≤ +12 dB) to avoid blowing up near-silent sources.
- If the knob crosses a side (left↔right) or returns to the centre, let the current grain finish and fade out via the boundary ramp — no clicks.

### Buffer swap (4x4 double-buffering)

- Each buffer owns its analysis (scores, period, RMS, rank, valid flag).
- When the freshly rendered buffer becomes the playing buffer, its analysis may still be in progress: pass through until it is valid, then engage with a boundary fade.
- The fill generator must never read from the buffer currently being rendered into.

## Parameters (propose final names/ranges to me first)

| Name | Range | Default | Notes |
|---|---|---|---|
| Mask | −100 … +100 % | 0 | left = keep clean, right = keep noisy, centre = off |
| Boundary fade | 1 … 50 ms | 10 | |
| Min run | 0 … 200 ms | 40 | |
| Grain size (noise) | 50 … 500 ms | 150 | right side only |

Optional, only if cheap: a display element showing the noisiness curve of the playing buffer with the masked frames highlighted and the playhead position.

## Constraints

- No allocation in `step()`. All analysis arrays are sized for the maximum buffer length and allocated through the plugin's normal memory requirements, in the appropriate memory region (big arrays in DRAM, per-frame scratch in fast memory).
- Keep per-`step()` analysis work bounded and tunable (frames per call constant).
- Float math throughout; no exceptions, no RTTI, no STL containers that allocate.
- Don't change existing behaviour when Mask is in the dead zone — it must be bit-identical pass-through.

## Verification

1. Desktop test harness for `noisemask.cpp`: a buffer of sine → white noise → sine → filtered noise. Print per-frame scores and ranks; confirm sine frames rank clean and noise frames rank noisy. Note that LPC will rate narrowband/resonant noise as fairly predictable — report where those frames land.
2. Render the harness output at Mask = −60 % and +60 % to WAV files so I can listen for clicks and level jumps at boundaries and loop points.
3. Measure worst-case cycles per `step()` for analysis and playback on the NT and report them.

Implement in stages, building and testing after each: (1) analysis + harness, (2) rank mapping + pass-through playback with mask display/debug output, (3) fill generator + crossfades, (4) buffer-swap integration, (5) parameters + UI polish.
