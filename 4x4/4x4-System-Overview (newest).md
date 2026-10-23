# 4×4 Matrix Feedback Generator — System Overview

A disting NT plugin that renders a self-oscillating 4-processor feedback
network offline (throttled, non-real-time) into short audio buffers
("kits"), which a real-time sampler engine then plays back as one-shots
or loops. There is no real-time connection between the plugin's input
and output — rendering and playback are decoupled by a double-buffered
kit store.

---

## 1. Signal network

Four channels, two 4×4 matrix mixers.

```
S1..S4 ──► [Injection mixer 4×4, unipolar] ──┐
                                             ├──► Σn ──► HPF ──► LPF ──► Pn ──► Reg ──┬──► Out n
P1..P4 ─► [Feedback mixer 4×4, bipolar] ─────┘                                        │
   ▲                                                                                  │
   └──────────────────────────────────────────────────────────────────────────────────┘
```

- **Injection mixer** (unipolar, 0…+1): four sources S1–S4 mixed independently into each channel. No inversion needed — sources aren't part of a loop.
- **Feedback mixer** (bipolar attenuverters, −1…+1): every processor output routed to every processor input. 16 crosspoints cover all 24 elementary loop types (4 self-loops, 6 two-processor, 8 three-processor, 6 four-processor).
- **Σn**: sums the injection mix and feedback mix for channel n.
- **HPF → LPF**: band-limits each channel before its processor; prevents loops from building energy outside the useful range. Ideally CV-controllable.
- **Pn**: four strongly nonlinear processors (frequency- and amplitude-dependent). One of the four is the frequency-domain pitch shifter (§2); another is the allpass-based frequency shifter (§3); a third is the geometric wavefolder (§4).
- **Reg** (regulator): DC block → AGC (drives loop gain toward 1, attack ~2–10ms, release ~100–500ms) → soft clip (tanh, never hard clip). Sits before the output split, so every loop and every output tap is protected.
- **Out n**: regulated output, split to the audio output and back into feedback-mixer column n.

## 2. Pitch-shift processor

One of the four processors implements frequency-domain pitch shifting, based on Ocean's algorithm (Juillerat, 2010) — the same technique as the reference implementation this design started from:

- 1024-sample analysis block, Hann window
- 4× overlap (hop size 256)
- 2× zero-padded spectrum (513 → 1025 bins) for the frequency-domain shift
- Bin shift applied via precomputed unity-root complex multiplication (no explicit phase-unwrapping/atan2 tracking across frames)
- Overlap-add resynthesis

The reference implementation used TI C28x hardware-accelerated FFT calls (`RFFT_f32`/`ICFFT_f32`). The disting NT API exposes **no FFT or DSP library** (verified against `api.h`, `wav.h`, `slot.h`, `serialisation.h`, `microtuning.h`) — the forward/inverse FFT must be hand-written in portable float C++ using only `<cmath>`, with statically-sized buffers.

This processor's block latency (at least one hop, realistically several for overlap-add to settle — several ms) becomes a structural property of whichever feedback loop it sits in, shaping that leg's timing/character rather than being a hidden implementation detail. This only affects the internal texture of the render — it has no bearing on playback, since playback never has to track the render's timeline in real time.

### FFT implementation: in-place, floating-point

- **In-place**: identical arithmetic to an out-of-place transform, so no audible difference either way — this is purely a memory/code-complexity choice. In-place wins here because the plugin's static-memory budget is already tight (kit buffers + up to 40 resident Contour generators, §7) and in-place halves the FFT's own buffer footprint at the cost of a bit-reversal permutation pass.
- **Floating-point (float32), not fixed-point**: this one does matter audibly. Float's rounding error stays proportional to signal level, so SNR stays roughly uniform as a signal decays; fixed-point's error is a constant absolute floor, so it gets relatively louder on quiet material. Because P1 sits inside a feedback loop, any noise it adds recirculates through the matrix and the other processors for the rest of the render, then gets baked permanently into the kit — which is then looped/triggered repeatedly, so it's not a one-off artifact. There's also no platform argument for fixed-point: the Cortex-M7 has a hardware FPU (`fpv5-d16`, hard-float ABI), the rest of the disting NT API is float-native throughout, and the reference implementation's own `RFFT_f32`/`ICFFT_f32` calls were already float.

### Parameter

- **Unit**: semitones. **Range**: ±24 (±2 octaves).
- It is one of the Contour-animated destinations, but unlike the 40 feedback/injection/cutoff crosspoints (§1, no manual base — curve output only), pitch shift has its own **base value** plus a separate **curve-amount (depth)** parameter: the curve offsets around the base rather than replacing it outright.
- The driving curve itself is bipolar, **±5V**.

## 3. Frequency-shift processor

A second processor implements true frequency shifting (every spectral component moves by a fixed Hz amount, unlike the ratio-based pitch shifter of §2) using an **IIR allpass-pair Hilbert transformer**, locked in 2026-10-06 after comparing it against the two other standard approaches:

- **FIR Hilbert transformer** — a single long linear-phase FIR (100+ taps for decent low-frequency accuracy) approximating a 90° phase shift across the band. Rejected: expensive per sample, and the cost scales directly with how low in frequency the shift needs to stay accurate.
- **FFT-domain analytic signal** — reusing P1's block FFT (zero the negative-frequency bins, double the positive ones, IFFT) to build the Hilbert-transformed signal, then multiply by a complex exponential and resynthesize via overlap-add. Rejected: comparable per-sample cost to P1 itself, needs its own sizeable complex scratch buffer (or careful time-sharing of P1's), and inherits P1's block-smeared, hop-latency character — making P2 feel like a second copy of P1's timing rather than a distinct loop leg.
- **IIR allpass-pair (chosen)** — two parallel chains of allpass sections (first/second-order) designed so the phase *difference* between the chains stays close to 90° across the audio band, feeding a rotating-phasor complex multiply (sideband selection via sign) and a real-part extraction. Recursive, sample-by-sample, a handful of sections per chain.

**Why this fits the plugin's execution model specifically**, not just DSP theory in the abstract:

- Far fewer operations per output sample than either alternative (~20–30 for allpass sections + phasor rotation + complex multiply, vs. ~100+ for FIR or ~80+ for FFT-domain) — directly helps bake time at any given peak-CPU-per-chunk ceiling (§6).
- Naturally sample-by-sample, so it chops into any chunk size the render state machine wants with none of the "must accumulate a full hop before producing output" bookkeeping both FFT-based options require (the same constraint P1 already has to live with).
- Negligible memory — a few state floats per allpass section, comfortably within the DTC pool (§7), vs. a second sizeable complex scratch buffer for the FFT-domain option.
- Very short, near-instant latency instead of inheriting P1's block-smeared character — gives this loop leg a genuinely different time-domain feel from P1 rather than a second copy of the same smearing.

**Open gap, not yet resolved**: the actual allpass coefficient set (how many sections per chain, their exact coefficients for acceptable low-frequency phase accuracy) needs to be sourced from a validated filter-design reference or derived properly during implementation — not fabricated. Parameter spec (shift unit/range, whether it's a Contour-animated destination like pitch shift, base + depth or curve-only) is also not yet defined, unlike P1's.

## 4. Wavefolder processor

A third processor implements Buchla/Serge-style wavefolding, with two parameters: **fold** (drive/depth into the folding stage) and **bias** (DC offset applied before folding, which asymmetrizes the fold and introduces even harmonics). Locked 2026-10-06:

- **Fold shape: geometric** — reflect the signal off hard boundaries (a `mod` + absolute-value construction, the classic "mirror" fold), rather than a sine-based smooth fold or a polynomial/Chebyshev-fitted curve. Chosen over the smoother alternatives for the harder, more metallic "West Coast" character the hard-cornered reflection gives, accepting that its discontinuous-derivative corners generate more high-frequency energy than a smooth curve would.
- **Evaluation: closed-form**, not a lookup table. A few multiply-adds per sample, no table memory, no regeneration concern even though `fold` and `bias` are expected to vary continuously (they're applied as pre-scaling/pre-offset on the input before the fixed nonlinearity — `y = foldShape(x · fold + bias)` — so the shape itself never needs rebuilding). Chosen specifically because DRAM/DTC are already overcommitted (§7); a LUT would have added another allocation against a budget that has none to spare, for a per-sample cost saving that's marginal against an already-cheap polynomial.
- **Aliasing: accepted as character, not mitigated.** No oversampling, no antiderivative antialiasing (ADAA) or other bandlimiting. Folding is the one processor among the four where this is a live design axis rather than a non-issue — the geometric shape's hard corners make it the most alias-prone of the four — but this plugin already treats other processors' artifacts as structural character rather than flaws to hide (P1's block latency, P2's phase-accuracy limits), so this is consistent with that stance rather than an exception to it.

**Two things this choice interacts with cleanly:**
- **Bias and Reg.** Bias deliberately shifts the signal before folding to shape harmonic asymmetry; any resulting steady DC component is removed by Reg's DC blocker downstream (§1), so bias doesn't need to worry about leaving a lasting offset in the final output.
- **Self-bounding in a feedback loop.** Unlike a bare gain stage, folding maps any input amplitude into a bounded range rather than diverging — a good property for a processor sitting inside a self-oscillating loop, on top of whatever bounding Reg's AGC/soft-clip already provides network-wide.

**Open gap, not yet resolved**: the exact geometric fold formula (period, corner placement, how `fold`/`bias` numerically map onto it) hasn't been derived yet — only the category of technique is fixed. Parameter spec (unit/range for `fold` and `bias`, whether either is a Contour-animated destination, base+depth or curve-only) is also undefined, same gap as P2's.

## 5. Execution model: render vs. playback

The plugin's `step()` does two jobs of very different weight:

1. **Player (always-on, real-time):** reads from the *current kit* buffer and plays slices as one-shots or loops. Comparable in cost to a basic sample-playback algorithm — this is the plugin's entire real-time obligation.
2. **Renderer (background, throttled):** advances the feedback-network computation (matrices, filters, processors, regulators) by one small bounded chunk per `step()` call — an explicit state machine, since the platform has no threads and `step()` must never block. Produces a fixed-length buffer (e.g. 5 seconds) at whatever pace the chunk size allows (render speed is a free design parameter, not fixed — e.g. ~4× slower than real-time is one possible operating point, not a requirement).

**Double buffering is required, not just the simplest option (locked 2026-10-05): playback must be gapless.** A *current kit* (playable) and a *next kit* (being rendered) are both kept fully resident. The renderer only ever writes into the inactive buffer; the pointer swap to make it the new *current kit* happens atomically, only once the render is fully complete. The player never sees a partially-rendered buffer.

This is close to the only sound approach for this plugin's playback model, not merely the easy one. A kit is rendered as one continuous 1–10s take (§8) and then **sliced and triggered randomly, likely polyphonically** — so there is no single predictable read position a leaner scheme (e.g. a rolling render that stays safely behind one read pointer, or double-buffering only the currently-changing segment) could rely on. Any slice can be hit at any moment, including rarely-used ones, including several overlapping at once, so the entire buffer has to be treated as potentially live for as long as it's the current kit. The only way to shrink this cost below full double buffering would be redesigning around independently-regenerable slices (each its own small buffer, refreshed on its own schedule) rather than one coherent continuous take chopped into related parts afterward — a different instrument, not a free optimization of this one.

Baked parameter-automation curves (pitch shift amount, feedback amplitude/polarity, HPF/LPF cutoffs, and others) are evaluated against the **renderer's own internal virtual sample-position counter**, not wall-clock time or `step()` call count — this keeps automation correct regardless of how fast or slow the render chunking happens to run.

## 6. CPU budget strategy

Because the renderer has no deadline (the player just keeps using whichever kit is current until a new one swaps in), render CPU cost is a tunable design parameter rather than an emergent property of the algorithm:

- **Always-on cost:** player only — small, constant, every `step()` call.
- **Render cost:** incurred only while actively baking a new kit; chunked into small bounded units (e.g. one FFT butterfly stage, or one stage of the matrix/filter/regulator pass) sized so that render-chunk cost + player cost + headroom for other algorithms in the same preset stays well under the module's CPU-overload threshold on *every single call* — average cost over a render cycle is not the relevant number, peak per-call cost is.
- Smaller chunks → lower peak CPU, longer bake time. Larger chunks → faster bake, higher (but still bounded and chosen) peak cost. This trade is free to make because nothing downstream has a deadline.
- Reference point: the LASER example plugin runs 20–30% CPU as a genuine real-time algorithm with a forced per-call cost. This design's forced per-call cost (the player) should be much lower, since the expensive part is optional and schedulable rather than forced every call — the actual number can only come from on-device profiling.

## 7. Memory

All allocation is static, sized via `calculateRequirements()` — no `new`/`malloc` at runtime anywhere, per the platform's hard constraint.

**Actual available pools** (sourced August 2026, not in any Expert Sleepers doc checked so far, unverified against current firmware — may have changed): **DTC 24KB, ITC 64KB, DRAM 512KB.** Almost certainly shared totals across every algorithm loaded in the same preset (the module runs up to 32 simultaneously), not a guarantee reserved for this plugin alone.

| Item | Approx. size | Notes |
|---|---|---|
| Kit buffer, double-buffered (required, §5) | format-dependent | Original 5s/mono/float32 spec ≈ 1.9MB double-buffered — **~3.8× the entire DRAM pool, not viable as specified.** E.g. 16-bit/48kHz ≈ 2.7s per buffer at the full 512KB pool; 12-bit packed/32kHz ≈ 5.46s per buffer. Exact format/duration still open — see below. |
| FFT/pitch-shifter scratch (in-place 2048-sample zero-padded complex buffer, overlap-add tail, window coefficients) | ~20–25KB | Too large for the 24KB DTC pool alongside other per-channel state — has to live in DRAM, not DTC. |
| Matrix coefficients, filter/regulator state | negligible, ~1KB | Comfortable fit in DTC. |
| Contour generators (up to 40 destinations) | ~520KB at full-size (13KB × 40), or ~48–60KB if trimmed (~1–1.5KB × 40) | Full-size alone exceeds the entire DRAM pool. Trimming `kMaxWin`/`kMaxEnv`/`kPieces` is numerically necessary now, not just efficient — decision still open. |
| Baked automation curve tables | unknown | Whether curves are pre-computed into stored tables or evaluated inline sample-by-sample via `Contour::process()` as the render advances is undecided; inline evaluation avoids duplicating kit-buffer-scale memory and is the more natural fit. |

## 8. Open design questions

- **State continuity between renders:** does the feedback network's internal state (filters, AGC envelopes, matrix history) carry over from the end of one render into the start of the next (continuous evolution), or does each render start cold with a fresh source injection (per the "kick-start loops" playing note)? Affects both sound character and per-render compute (settling time).
- **Render chunk granularity and resulting bake time:** only determinable by on-device profiling.
- **Kit buffer bit-depth / sample-rate / duration:** real tradeoff against the 512KB DRAM ceiling once Contour and FFT scratch are also accounted for — not decided. Paused pending research into a possible additional disting NT memory feature that could change the available headroom.
- **Contour generator sizing:** full-size vs. trimmed per-instance footprint — paused for the same reason.
- **Sampler slicing method:** overall model is settled — one continuous 1–10s render, sliced afterward, triggered randomly and likely polyphonically (this is what makes double buffering effectively mandatory, §5) — but fixed-size vs. transient-detected slicing, exact slice count, and polyphony limits are still unspecified.
- **AGC vs. pitch-shifter latency interaction:** the AGC's fast attack (~2–10ms) operates in a similar time range to the pitch-shift leg's inherent block latency; possible periodic pumping tied to hop boundaries needs listening/tuning, not just calculation.
- **Kit-swap continuity:** double buffering guarantees no torn/partial reads, but not a smooth musical transition — if a loop is mid-playback when a freshly-baked kit swaps in, the waveform content can jump abruptly at that instant even though nothing glitches technically. Separate question from buffer safety, not yet addressed.
- **Frequency-shifter (P2) allpass coefficients and parameter spec:** approach is locked (§3), but the actual coefficient set and the shift parameter's unit/range/Contour-destination treatment are not yet defined.
- **Wavefolder (P3) exact fold formula and parameter spec:** shape/evaluation/aliasing stance is locked (§4), but the precise geometric formula and `fold`/`bias` unit/range/Contour-destination treatment are not yet defined — same category of gap as P2's.

## 9. Platform constraints honored

- No dynamic allocation — placement `new` on firmware-provided static memory only.
- No exceptions, no RTTI, no STL, no threads, no OS/file calls inside `step()`.
- FFT implemented from scratch in portable float C++ (`<cmath>` only) since the API provides none.
- `step()` never blocks — render work is chunked as an explicit state machine, never a single long pass.
