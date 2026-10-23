# 4×4 Matrix Feedback Generator — System Overview

A disting NT plugin that renders a self-oscillating 4-processor feedback
network offline (throttled, non-real-time) into short audio buffers
("kits"), which a real-time sampler engine then plays back as one-shots
or loops, sliced and triggered randomly and likely polyphonically. There
is no real-time connection between the plugin's input and output —
rendering and playback are decoupled by a double-buffered kit store.

---

## 1. Signal network

Four channels, one 4×4 feedback matrix mixer, and four fixed injection
points (the earlier 4×4 injection mixer has been removed).

```
Sn ──────────────────────────────────────────┐
 (fixed, direct — no injection mixer)         ├──► Σn ──► Bandpass ──► Pn ──► Reg ──┬──► Out n
P1..P4 ─► [Feedback mixer 4×4, bipolar] ──────┘                                     │
   ▲                                                                                │
   └────────────────────────────────────────────────────────────────────────────────┘
```

- **4 fixed injection points**: each source Sn feeds directly and only into its own channel's Σn. No gain stage, no adjustable level, not a modulation destination. Each Sn's actual content is a math-generated square-wave burst, not live or recorded audio — see "Starter wave injection" below.
- **Feedback mixer** (bipolar attenuverters, −1…+1): every processor output routed to every processor input. 16 crosspoints cover all 24 elementary loop types (4 self-loops, 6 two-processor, 8 three-processor, 6 four-processor).
- **Σn**: sums the fixed injection and the feedback mix for channel n.
- **Bandpass**: a per-channel resonant bandpass band-limits each channel before its processor (**replaced the former HPF→LPF pair, 2026-10-20 user request** — the one-poles were too gentle to be clearly effective). Center frequency (Hz) + bandwidth (semitones, 1 st … 8 octaves), **static per-channel base values on the Filters page by default**, not Contour-modulated (§6/§9). Being inside the loop, it strongly constrains which band each Pn — and the self-oscillation — can sustain. **Two optional Filters-page toggles modulate it (added 2026-10-21, both default Off):** **VCFRND** — when On, each render draws fresh seed-derived (reproducible) random center frequencies (log-uniform 20–16000 Hz) per channel, overriding the static Freq params; **QCONT** — when On, one shared Contour curve drives all 4 channels' bandwidth ("Q") per internal sample (biquad coeffs recomputed every sample), overriding the static BW params. With both Off the filters are exactly as before.
- **Pn**: four strongly nonlinear/spectral processors. **P1** is the frequency-domain pitch shifter (§2). **P2** is the allpass-based frequency shifter (§3). **P3** is a plain time delay (§4 — the geometric wavefolder was omitted 2026-10-19). **P4** is a **12-stage allpass phaser** (§5 — the bitcrusher was omitted 2026-10-21 per user request; "just use its phase shifter with 12 stages as a processor"). It is the phase-shifter core only — no internal feedback, no LFO of its own; the center-frequency sweep comes from the Contour system like every other processor's modulation.
- **Reg** (regulator): DC block → AGC → soft clip (tanh, never hard clip). Sits before the output split, so every loop and every output tap is protected. **AGC target locked 2026-10-18: −18dB to −6dB, relative to the hardware's ±11V absolute output ceiling** — ≈1.39V to ≈5.51V peak. Attack ~2–10ms, release ~100–500ms (unchanged from the original design notes); peak vs. RMS detection still open.
- **Out n**: regulated output, split to the audio output and back into feedback-mixer column n.

### Starter wave injection

What actually feeds S1–S4, locked 2026-10-18 after working through several alternatives (live-captured audio, a Contour-generated wavetable) and settling on the simplest: a **math-generated square wave**, not live or recorded audio at all.

- **Algorithm**: a plain phase accumulator per channel (increment by `freq/sampleRate` each render sample, wrap at 1.0) compared against a threshold — the cheapest possible waveform generator, no lookup table, no engine state beyond one phase float per channel.
- **4 independent channels**: each of S1–S4 has its own **frequency** and its own **duration**, both real, static `_NT_parameter` entries (performer-set once, not Contour-animated — a one-shot burst this short doesn't have much room for a slow Contour envelope to meaningfully animate within it anyway). 8 new parameters total (4× freq, 4× duration).
- **Duration**: 1–500 ms per channel, independently settable (min lowered to 1 ms, 2026-10-19).
- **Timing**: fires once, at the very start of each render's virtual timeline (confirmed — "once per render, at the start"), then silence for the rest of that channel's Sn input. No live audio capture, no recording buffer, no mandatory real-time wait before the throttled render computation begins — this mechanism keeps the plugin's original premise intact: **no real-time connection between the plugin's input and output**, since nothing here reads a physical/bus input at all.
- **Memory/CPU impact**: negligible — 4 phase floats + 4 frequency + 4 duration values (≈128 bytes), comfortably within DTC. No Contour instances involved, so this doesn't add to the confirmed 21 Contour-driven destinations or touch the locked kit-buffer memory budget (§8) at all.

<!-- FIG:STARTER -->

### Implementation status (Step 10, built and validated 2026-10-18)

`feedback_network.h` implements the matrix, fixed injection points, and starter wave exactly as specified above. Each of the 4 channels is hard-wired to one specific processor type matching §2-§5's naming: channel 1 = P1, channel 2 = P2, channel 3 = P3, channel 4 = P4 (not 4 interchangeable instances of one generic processor). Each processor's own control input (depth/fold+bias/bits+divisor) is a plain scalar field on a `ChannelControls` struct for now — Step 12 replaces these with Contour-driven values.

- **Feedback timing**: a digital feedback loop can't resolve a same-sample circular dependency without iterative solving, which isn't called for here. The necessary structural choice — a one-sample delay in the loop, where each channel's feedback mix at sample *t* uses every channel's Reg output from sample *t-1* — isn't a deviation from the design; it's the concrete form any sample-by-sample digital feedback network has to take (an analog patch has the same property implicitly, since signals take nonzero time to propagate through a cable).
- **Validated on desktop** (`test_feedback_network.cpp`): the starter wave fires for exactly its set duration then goes silent; the matrix correctly uses the previous sample's outputs (confirmed via a silenced-starter + nonzero-self-feedback case, where the very first processed sample shows zero feedback contribution, not some resolved nonzero fixed-point value); a 1-second run with strong (0.95) self-feedback on the P1 channel produces no NaN and no runaway divergence, and — the core premise of the whole plugin — genuinely sustains a nonzero signal well after the starter burst ends, confirming real self-oscillating feedback behavior, not just a pass-through. ARM cross-compile is clean.
- **Real bug caught during validation, not in this step's own code**: the test initially measured exactly 0.0 on every sample, which traced back to every standalone trace script (not the processors themselves) forgetting to call `fft::initTwiddles()` before using a channel-1 (P1) processor — the twiddle table silently stays all-zero if this is skipped, so every FFT output, and therefore every P1 output, comes out exactly 0.0 with no error of any kind. This is now documented directly in `p1_pitchshift.h` and `feedback_network.h`'s file headers as a flagged gotcha, since it will matter again in Step 15 when the factory's `initialise()` callback becomes the one real place this has to be called.

### Implementation status (Step 11, built and validated 2026-10-18): HPF/LPF/Reg completed

`filters.h` implements the per-channel HPF→LPF pair and the full Reg chain (DC block → AGC → soft clip), now wired into `feedback_network.h` completing the chain Σn → HPF → LPF → Pn → Reg → Out n, with Reg's output (not Pn's raw output) feeding both the audio output and the feedback matrix — Step 10's matrix is now fully completed, not provisional.

- **HPF/LPF**: standard one-pole (6dB/octave) filters, cutoffs taken as plain scalar fields on `ChannelControls` for now (8 total — one HPF + one LPF per channel, matching the "8 filter cutoffs" tracked in §9), Contour-driven in Step 12. **One real bug caught and fixed during validation**: the first HPF attempt used the "complementary" trick (`x - lowpass(x)`), which measured numerically wrong (gain ~0.66 instead of 0.707 at the nominal cutoff, and capped around ~0.93 instead of approaching 1.0 well above cutoff). Root cause, found by direct transfer-function evaluation: that recursion is algebraically identical to `y[n] = a·(y[n-1] + x[n] - x[n-1])`, whose own passband ceiling at Nyquist is `2a/(1+a)` — strictly below 1.0 for any `a < 1`. Fixed with the standard normalized one-pole highpass `y[n] = (1+a)/2·(x[n] - x[n-1]) + a·y[n-1]`, which forces unity gain at Nyquist for any `a`. Re-verified: HPF and LPF both hit -3dB at the intended cutoff and the HPF now passes 10x-cutoff content at >0.99 gain.
- **Reg**: DC blocker (classic `y[n] = x[n] - x[n-1] + R·y[n-1]`, R=0.995) → AGC (peak envelope follower, attack 5ms / release 250ms, both within the locked 2-10ms/100-500ms ranges) → soft clip (`11·tanh(x/11)`, provably bounded to the hardware's ±11V ceiling for any input, never a hard clip). AGC targets a single level — **-12dB relative to the ±11V ceiling (≈2.763V), the midpoint of the locked -18..-6dB window** — a deliberate simplification over a dual-threshold deadband, documented here rather than silently chosen.
- **Peak vs. RMS detection — explicitly still open, flagged per the plan rather than silently decided.** Peak detection is what's implemented (the plan names it "the simpler default"), but this has NOT been confirmed by ear on real self-oscillating content. RMS would react less to single-sample transients and more to sustained loudness, which could matter a lot for how "pumpy" the AGC feels on this specific signal path. **Needs a listening pass after Step 16's on-device testing** before being considered settled.
- **Validated on desktop** (`test_filters.cpp`, `test_feedback_network.cpp`): HPF/LPF frequency response confirmed via sine sweep (3 points each); DC blocker removes a sustained 3V offset to <0.01V; soft clip never exceeds ±11V across a wide sweep and is near-identity for small input; the full Reg chain, fed a 20V-amplitude + 2V-DC test signal (deliberately far outside any reasonable operating range), settles output peak to 2.838V — inside the locked target band and close to the exact 2.763V target — within the measured 0.5-1.0s window. Re-running the full feedback-network test with Reg now in the loop shows the previously-unbounded channel-1 self-oscillation (peak ≈6.9V before Reg) settles to ≈2.85V after Reg, right in the AGC's target zone — direct confirmation the regulator is doing its job on the actual self-oscillating signal path, not just on a synthetic test tone. ARM cross-compile is clean.

## 2. Pitch-shift processor (P1)

> **REWRITTEN 2026-10-22 — see §12.** P1 was a monophonic single-partial tracker (details below, kept as history); it is now a **full-spectrum** Ocean bin-shift (all bins, no atan2) carrying the BlockSize/Swaps/SwapSize/Omit spectral manipulation. The randomized per-render base pitch + Contour depth are unchanged; only the spectral processing changed. The narrative below documents the original single-partial implementation.

Implements frequency-domain pitch shifting, based on Ocean's algorithm (Juillerat, 2010) — the same technique as the reference implementation this design started from:

- 1024-sample analysis block, Hann window
- 4× overlap (hop size 256)
- 2× zero-padded spectrum (513 → 1025 bins) for the frequency-domain shift
- Bin shift applied via precomputed unity-root complex multiplication (no explicit phase-unwrapping/atan2 tracking across frames)
- Overlap-add resynthesis

The reference implementation used TI C28x hardware-accelerated FFT calls (`RFFT_f32`/`ICFFT_f32`). The disting NT API exposes **no FFT or DSP library** (verified against `api.h`, `wav.h`, `slot.h`, `serialisation.h`, `microtuning.h`) — the forward/inverse FFT must be hand-written in portable float C++ using only `<cmath>`, with statically-sized buffers.

This processor's block latency (at least one hop, realistically several for overlap-add to settle — several ms) becomes a structural property of whichever feedback loop it sits in, shaping that leg's timing/character rather than being a hidden implementation detail. This only affects the internal texture of the render — it has no bearing on playback, since playback never has to track the render's timeline in real time.

### FFT implementation: in-place, floating-point

- **In-place**: identical arithmetic to an out-of-place transform, so no audible difference either way — this is purely a memory/code-complexity choice. In-place wins here because the plugin's static-memory budget is already tight (kit buffers + resident Contour generators, §8–§9) and in-place halves the FFT's own buffer footprint at the cost of a bit-reversal permutation pass.
- **Floating-point (float32), not fixed-point**: this one does matter audibly. Float's rounding error stays proportional to signal level, so SNR stays roughly uniform as a signal decays; fixed-point's error is a constant absolute floor, so it gets relatively louder on quiet material. Because P1 sits inside a feedback loop, any noise it adds recirculates through the matrix and the other processors for the rest of the render, then gets baked permanently into the kit — which is then looped/triggered repeatedly, so it's not a one-off artifact. There's also no platform argument for fixed-point: the Cortex-M7 has a hardware FPU (`fpv5-d16`, hard-float ABI), the rest of the disting NT API is float-native throughout, and the reference implementation's own `RFFT_f32`/`ICFFT_f32` calls were already float.

### Parameter

- **Unit**: semitones. **Range**: ±24 (±2 octaves).
- It is one of the Contour-animated destinations, but unlike the fixed-rate crosspoint/cutoff/P4 destinations (curve output only, no manual base), pitch shift has a **base value** plus a separate **curve-amount (depth)** parameter: the curve offsets around the base rather than replacing it outright.
- **Base is randomized at each new render, not performer-set — locked 2026-10-18.** Chosen fresh (uniformly within the full ±24 semitone range) at the start of every kit generation, then held fixed for that render's whole duration. No longer a real `_NT_parameter` — it's internal state (§9). **Depth remains a real, performer-facing parameter.** This is the general rule going forward for any future base+depth-style parameter — **P2 did end up using this treatment (§3, Step 7); P3 did not (§4, Step 9 — fold/bias are both curve-only, no base, same as P4)** — base is always randomized per render, depth is always a real parameter, whenever a processor does use base+depth.
- **Open**: whether this randomization is seed-derived (reproducible — same seed always gives the same base, consistent with how every other random element in this design works) or drawn from true hardware entropy (breaking reproducibility for this one value) isn't specified yet (§10).
- The driving curve itself is bipolar, **±5V**.

### Implementation status (Step 4, built and validated 2026-10-18) — deviates from the above in three ways

The original paper (Juillerat, 2010) wasn't available to verify the exact "unity-root bin multiplication, no phase tracking, 2× zero-padded spectrum" mechanics described above against. What's actually implemented in `p1_pitchshift.h`, reached after three attempts validated against `test_p1.cpp` (known input frequency must shift by the expected ratio, measured via FFT-peak and zero-crossing analysis of the output):

1. **Fixed per-hop phase rotation, 2× zero-padded, no tracking** (closest attempt at the description above) — produced **no pitch shift at all**. A constant phase offset per hop changes starting phase, not reconstructed frequency.
2. **Classical phase-vocoder tracking, reconstructed back into the same analysis bin** — correct at ratio=1.0 (<0.3% error) but badly wrong at every other ratio (40–85% error, direction sometimes inverted). Root cause: a single bin's inverse-FFT contribution is a sinusoid at that bin's own native frequency; overlap-add can only bend the apparent output frequency a limited distance from that native center (roughly the Hann window's main-lobe width) before reconstruction breaks down. An octave shift needs a ~458Hz bend from a 421.875Hz-native bin — far outside that range.
3. **What's implemented — "bin-shift" phase vocoder**: the tracked partial's magnitude and phase-vocoded instantaneous frequency are written into the output bin whose *native* frequency is already close to the shifted target (`newBin = floor(peakBin * ratio)`), so only a small residual correction is needed. Validated to <0.5% frequency error across ±1 octave and non-octave ratios.

This means: **phase tracking is real and required** (previous-analysis-phase + synthesis-phase-accumulator state, ~4KB for 513 bins, with a bounded per-hop wrap) — not the "no explicit phase-unwrapping/atan2 tracking" of the original description. The **2× zero-padding is dropped** — not needed once reconstruction targets the correct native bin.

**Remaining honest simplification, unchanged from the original design intent but now concretely confirmed**: only the single dominant spectral bin is tracked and shifted each hop; every other bin — including real signal energy that Hann-window leakage spreads into neighbouring bins — is zeroed. This is a monophonic/single-partial pitch tracker, validated only against single test tones (what the stated criterion calls for). Two concrete, measured consequences:
- **Polyphonic/noisy input is not gracefully handled** — energy outside the tracked partial is dropped, not shifted.
- **Amplitude loss is inherent and frequency-dependent**, not a wrong overlap-add scale constant: for a 440Hz input sitting roughly halfway between bins 9 and 10, the discarded neighbour bin carries nearly as much magnitude as the kept peak bin, so output measures ~0.71× input amplitude at that frequency (varies with how closely the input aligns to a bin center — closer alignment loses less). Cannot be fixed by a bigger fixed multiplier without causing overshoot for inputs that land exactly on a bin.

Both are flagged here rather than hidden; revisit if polyphonic/noisy content through this loop leg needs better fidelity once heard on hardware (Step 16).

## 3. Frequency-shift processor (P2)

A second processor implements true frequency shifting (every spectral component moves by a fixed Hz amount, unlike the ratio-based pitch shifter of §2) using an **IIR allpass-pair Hilbert transformer**, locked 2026-10-06 after comparing it against the two other standard approaches:

- **FIR Hilbert transformer** — a single long linear-phase FIR (100+ taps for decent low-frequency accuracy) approximating a 90° phase shift across the band. Rejected: expensive per sample, and the cost scales directly with how low in frequency the shift needs to stay accurate.
- **FFT-domain analytic signal** — reusing P1's block FFT (zero the negative-frequency bins, double the positive ones, IFFT) to build the Hilbert-transformed signal, then multiply by a complex exponential and resynthesize via overlap-add. Rejected: comparable per-sample cost to P1 itself, needs its own sizeable complex scratch buffer (or careful time-sharing of P1's), and inherits P1's block-smeared, hop-latency character — making P2 feel like a second copy of P1's timing rather than a distinct loop leg.
- **IIR allpass-pair (chosen)** — two parallel chains of allpass sections (first/second-order) designed so the phase *difference* between the chains stays close to 90° across the audio band, feeding a rotating-phasor complex multiply (sideband selection via sign) and a real-part extraction. Recursive, sample-by-sample, a handful of sections per chain.

**Why this fits the plugin's execution model specifically**, not just DSP theory in the abstract:

- Far fewer operations per output sample than either alternative (~20–30 for allpass sections + phasor rotation + complex multiply, vs. ~100+ for FIR or ~80+ for FFT-domain) — directly helps bake time at any given peak-CPU-per-chunk ceiling (§7).
- Naturally sample-by-sample, so it chops into any chunk size the render state machine wants with none of the "must accumulate a full hop before producing output" bookkeeping both FFT-based options require (the same constraint P1 already has to live with).
- Negligible memory — a few state floats per allpass section, comfortably within the DTC pool (§8), vs. a second sizeable complex scratch buffer for the FFT-domain option.
- Very short, near-instant latency instead of inheriting P1's block-smeared character — gives this loop leg a genuinely different time-domain feel from P1 rather than a second copy of the same smearing.

**Allpass coefficient set — resolved 2026-10-18 (Step 6).** An initial attempt recalled a coefficient set from memory, believed to be a well-known published design. Per this file's "not fabricated" rule, that recollection was checked numerically before being trusted — it failed (computed phase difference nowhere near 90° across the band) and was discarded; memory of an unverified number is not a validated source. What's actually used instead was **derived via nonlinear least-squares optimization** (Levenberg-Marquardt, numerical Jacobian, 40 random restarts) directly against the stated design criterion — phase difference between the two chains held to 90° across the audio band — then independently re-verified by direct frequency-response computation. Full method and reproducible derivation: `design_hilbert.cpp`.

- **Structure**: two chains, 6 first-order allpass sections each, each section `H(Z) = (a + Z) / (1 + a·Z)` with `Z = z⁻²`. **Critical structural finding, confirmed before relying on it**: with the naive phaseA−phaseB objective, the fit is excellent below ~2kHz but the error explodes to ~180° by 20kHz (`Z` depending on `2ω` means each section completes an extra cycle within the audio band). Fixed by giving chain A's output one extra full-rate sample of delay relative to chain B's output — baked into the optimization objective, not a post-hoc patch — which resolves the fit across the entire Nyquist range. **Chain A's output must carry this extra one-sample delay in the Step 7 implementation.**
- **Coefficients** (seed 12345, deterministic — rerunning `design_hilbert.cpp` reproduces them exactly):
  - Chain A: `-0.9983674986  -0.2712161970  -0.9887756210  -0.9625251036  -0.6730934176  -0.8841466094`
  - Chain B: `-0.8019822326  -0.4900702536  -0.9336325065  -0.0787602760  -0.9945280131  -0.9791624864`
- **Verified accuracy**: worst-case error 0.119° across the full [20Hz, 20kHz] design band (most of the band under 0.05°). Usable range extends further in practice — under 1° error from ~16Hz up to ~23.5kHz; below ~16Hz it degrades quickly (7.97° at 10Hz, 30.8° at 5Hz). This is the "allpass Hilbert approximations typically degrade at the low end" caveat this section originally anticipated, now with concrete measured numbers: **usable low-frequency bound ≈16Hz** (sub-1° accuracy), well clear of the audio band's practical low end.

**Parameter spec and implementation — resolved 2026-10-18 (Step 7).** Settled as **base + depth, the same treatment as P1** (§2): a fresh random base shift drawn at the start of each render (bipolar), held fixed for that render, plus a real performer-facing depth parameter offsetting around it — chosen so P2 gets the same "every render sounds like a different instrument" character P1's randomized base gives, rather than starting neutral like P4's curve-only controls.

- **Unit**: Hz, bipolar. **Range**: ±2000Hz for the combined base+depth total — a deliberately wide, dramatic range (true frequency shifting turns harmonic content inharmonic within a few tens of Hz, so even this is already extreme over most of its range), sized comparably "wide but bounded" to P1's ±24 semitones, and chosen to stay comfortably inside the allpass design's verified <1°-accurate range (~16Hz–23.5kHz, §3 above) — it's the *shift amount* that's bounded here, not the input signal's own frequency, so ±2000Hz never asks the Hilbert approximation to operate outside its validated range.
- **Structure implemented** (`p2_freqshift.h`): both allpass chains (6 sections each, Step 6 coefficients) process the same input sample; chain A's output carries the extra one-sample delay Step 6 found necessary; a rotating phasor (absolute phase accumulator, `sinf`/`cosf` per sample — simpler and more robust than an incremental-rotation cache given the shift can be modulated continuously by a Contour curve every sample, at the cost of two transcendental calls/sample) performs the complex multiply; the real part is extracted. Entirely sample-by-sample, no block latency, as designed.
- **Validated on desktop** (`test_p2.cpp`): a test tone's spectral peak moves by the expected *fixed Hz amount* (not a ratio) to within ~1-2Hz across five shift settings spanning ±1000Hz, confirming both magnitude and direction. One real bug caught and fixed this way: the first implementation had the complex-multiply sign backward (output tracked `inputFreq − shiftHz` instead of `inputFreq + shiftHz`) — a clean sign flip, visible immediately as the ±200Hz test cases swapping expected outcomes, fixed by flipping one sign in the real-part extraction. ARM cross-compile (Cortex-M7, same flags as the rest of the project) is clean; `sizeof(Processor)` is 256 bytes (12 allpass sections × ~20 bytes of state each, plus the phase/base/delay scalars) — comfortable in the DTC pool alongside the other processors' negligible state (§8).

## 4. P3 processor — delay (was: wavefolder)

**SUPERSEDED 2026-10-19 (user request): P3 is now a plain time delay, not a wavefolder.** The
delay (`delay.h`) ranges **1–100 ms** and follows the P1/P2 base+depth model: a randomized
per-render base delay time, plus a Contour-driven, depth-scaled offset (±50 ms) around it, clamped
to 1–100 ms; the **P3 depth** param (Depth page) scales that modulation. Its ring buffer is sized
for 100 ms at the 48 kHz base rate; **max delay time scales down with oversampling** (≈100 ms @1×,
≈25 ms @4×) — a pure delay doesn't alias, so there's no fidelity cost, only this buffer-length
scaling (validated in `test_delay.cpp`). The wavefolder design below is retained for reference and
its code (`p3_wavefolder.h`) still exists/tests, but is **no longer wired into the signal path**.

<details>

### (Former) wavefolder design — reference only, not in the signal path

A third processor implements Buchla/Serge-style wavefolding, with two parameters: **fold** (drive/depth into the folding stage) and **bias** (DC offset applied before folding, which asymmetrizes the fold and introduces even harmonics). Locked 2026-10-06:

- **Fold shape: geometric** — reflect the signal off hard boundaries (a `mod` + absolute-value construction, the classic "mirror" fold), rather than a sine-based smooth fold or a polynomial/Chebyshev-fitted curve. Chosen over the smoother alternatives for the harder, more metallic "West Coast" character the hard-cornered reflection gives, accepting that its discontinuous-derivative corners generate more high-frequency energy than a smooth curve would.
- **Evaluation: closed-form**, not a lookup table. A few multiply-adds per sample, no table memory, no regeneration concern even though `fold` and `bias` are expected to vary continuously (they're applied as pre-scaling/pre-offset on the input before the fixed nonlinearity — `y = foldShape(x · fold + bias)` — so the shape itself never needs rebuilding). Chosen specifically because DRAM/DTC are already overcommitted (§8); a LUT would have added another allocation against a budget that has none to spare, for a per-sample cost saving that's marginal against an already-cheap polynomial.
- **Aliasing: character by default, now optionally mitigated via oversampling (2026-10-19).** The folder has no per-stage ADAA/bandlimiting, so at 1× (default) its hard corners alias freely — the original "aliasing as character" stance. But the renderer now exposes a global **Oversample** factor (1×/2×/4×/8×, §7) that runs the whole network — P3 included — at a higher internal rate before an anti-aliased downsample to the 32kHz kit; P3 is the primary beneficiary (measured: 4× cuts the folder's in-band alias energy ~18×). So folding aliasing is now the user's choice: raw/gritty at 1×, progressively cleaner as the factor goes up. The other processors' artifacts (P1 block latency, P2 phase limits, P4 quantization) remain as before unless oversampled.

**Two things this choice interacts with cleanly:**
- **Bias and Reg.** Bias deliberately shifts the signal before folding to shape harmonic asymmetry; any resulting steady DC component is removed by Reg's DC blocker downstream (§1), so bias doesn't need to worry about leaving a lasting offset in the final output.
- **Self-bounding in a feedback loop.** Unlike a bare gain stage, folding maps any input amplitude into a bounded range rather than diverging — a good property for a processor sitting inside a self-oscillating loop, on top of whatever bounding Reg's AGC/soft-clip already provides network-wide.

**Exact fold formula — resolved 2026-10-18 (Step 8).** Derived directly from the "mod + absolute-value construction" described above, then independently verified by direct numerical computation (not just asserted) — see `verify_fold.cpp`:

```
wrap4(a)      = a - 4*floor(a/4)              // always-nonnegative mod into [0,4)
foldShape(u)  = 1 - | wrap4(u + 1) - 2 |       // period 4, range [-1,1]
y             = foldShape(x · fold + bias)     // per this section's existing mapping
```

- **Period 4**, identity on `u ∈ [-1,1]` (`foldShape(u) = u` exactly, no deviation — confirmed numerically), reflecting at every odd integer boundary (`±1, ±3, ±5, ...`): e.g. `foldShape(1.5) = 0.5`, `foldShape(2) = 0`, `foldShape(3) = -1`, continuing the zigzag indefinitely.
- **Self-bounding, verified for any input**: swept `u` across `[-1000, 1000]`, max `|foldShape(u)|` measured at exactly `1.0` — confirms the §4 reliance on bounded output holds for arbitrarily large pre-scaled/pre-offset input, not just near the origin.
- **Corner count scales directly with `fold`**: since `foldShape` has a fixed period-4 shape, increasing `fold` (which scales `x` before folding) packs proportionally more period-4 cycles into the same input amplitude range — i.e. more corners, more harmonics, the classic "more drive = more folds" Buchla/Serge behavior — without changing `foldShape` itself at all. `bias` shifts where in that fixed shape a given input amplitude lands, asymmetrizing the fold exactly as this section's existing text describes.

**Parameter spec and implementation — resolved 2026-10-18 (Step 9).** Settled as **fold and bias both curve-only Contour destinations, no base value — the same treatment as P4 (§5), not P1/P2's base+depth.** Rationale: fold/bias are "amount of effect" controls (how much drive, how much asymmetry), the same category as P4's bit-depth/rate-divisor, not "where in pitch/frequency space" controls like P1/P2's base — and at `fold=0` the output degenerates to a constant (`foldShape(bias)` alone, no signal dependence at all), so zero is already the natural, musically neutral starting point for a curve to animate away from, same as P4's curve-only controls starting from no effect. A randomized per-render base wouldn't add anything meaningful here.

- **Curve-to-destination mapping** (documented numerically here since this is the first processor where it needed spelling out): each Contour instance's driving curve is bipolar ±5V (established convention, §2). Both map linearly from that range: `fold`: −5V→0.0, +5V→20.0 (unitless gain/multiplier); `bias`: −5V→−4.0, +5V→+4.0 (unitless, matches the fold formula's period-4 domain exactly).
- **Implemented** (`p3_wavefolder.h`) as pure stateless free functions, not a class — this processor is a memoryless nonlinearity with no persistent state at all, unlike P1/P2/P4, so a class/instance would be pointless ceremony. `sizeof` consideration doesn't apply; ARM cross-compile produces zero `.bss`.
- **Validated on desktop** (`test_p3.cpp`): confirmed `process(x, 1.0, 0.0)` reduces exactly to Step 8's verified `foldShape(x)`; small `fold` stays near-linear (identity region); larger `fold` produces proportionally more sign changes (more folds) over the same input sweep; nonzero `bias` breaks odd symmetry (introduces the even harmonics this section's existing text describes) while `bias=0` keeps it exactly odd; output stays within `[-1,1]` across a wide `fold`/`bias`/`x` sweep; `fold=0` degenerates to a constant exactly as the rationale above predicts. The plan's prompt asked to check against "§4's Fig. 4" — no such figure actually exists in this document (checked directly), so validation instead targeted Step 8's independently-verified formula, which is the authoritative source for the shape.

</details>

## 5. 12-stage allpass phaser processor (P4)

The fourth processor is a **12-stage allpass phaser** (`p4_phaser.h`). It **replaced the bitcrusher / sample-rate-reducer on 2026-10-21** at the user's request: *"omit the bitcrusher and add a 12 stage phaser instead like dual phaser but 12 stages … just use its phase shifter with 12 stages as a processor instead of the crusher."* Only the phase-shifter core is used — **no internal feedback, no LFO/trigger of its own** (the plugin's Contour system supplies the sweep). The former bitcrusher spec is preserved in git / `History` and `p4_bitcrusher.h` is kept on disk, unreferenced, in case it is wanted back.

### Structure

- **12 cascaded first-order allpass sections**, all sharing one center frequency fc. Each section is `H(z) = (a + z⁻¹)/(1 + a z⁻¹)`, i.e. `y[n] = a·x[n] + x[n-1] − a·y[n-1]`, with
  **`a = (t − 1)/(t + 1)`, `t = tan(π·fc/fs)`.** This is the coefficient that puts the section's −90° quadrature point exactly at fc (from `Re{H(e^{jωc})}=0 ⇒ cos ωc = −2a/(1+a²)`). The inverted sign `(1−t)/(1+t)` would mirror fc around fs/4 and sweep the comb backwards — **a real bug that shipped briefly and was caught by the sweep-direction test** (`test_p4_phaser.cpp`), then fixed.
- `a ∈ (−1, 1)` for all `0 < fc < fs/2`, so every section — and the cascade — is **unconditionally stable** at any center frequency, for any oversampling rate.
- Each section is magnitude-flat (allpass); the audible **phaser notches** appear only in the **dry+wet mix**: `out = 0.5·(dry + wet)`, `wet` = the 12-stage output. The total allpass phase runs 0 → −12π across 0 → fs/2, so the mix magnitude is **`|cos(6·φ₁(f))|` — exactly SIX notches** that slide with fc (12 stages ⇒ 6 notches; verified analytically and against the measured response in `test_p4_phaser.cpp`).

### Control model (base + depth, like P1/P2)

- **Center frequency fc** is the modulated quantity (the phaser's "LFO target"). A **randomized per-render base** is drawn in `[300, 1800] Hz` (seed-derived, reproducible), plus a **Contour-driven, depth-scaled signed Hz offset** (`±2000 Hz`, the P4 Contour destination, §6). `fc` is clamped to `[40 Hz, min(8000 Hz, 0.45·fs)]`. **"P4 depth"** (Depth page) scales the sweep. This is the P1/P2 base+depth treatment, not the old curve-only one — the single swept parameter replaced the bitcrusher's two curve-only destinations (bits, divisor), so the Contour set dropped by one (22 → 21, §6/§8).

### Why this fits the plugin's execution model

- **Trivial state**: 12 allpass `(x1,y1)` pairs + one base-freq float — ~100 bytes, negligible.
- **Sample-by-sample, no block latency** — chops into any render-chunk size, like P2.
- **Self-bounding inside the feedback loop**: the dry+wet mix has `|H(f)| ≤ 1` for every frequency, so a static setting cannot amplify; a smoothly Contour-swept fc stays bounded (gradual coefficient change ⇒ negligible energy injection), and Reg contains any transient regardless. A **denormal flush** on each section's recursive state mirrors the feedback loop's (Cortex-M7 denormal-spike insurance).
- **Oversample-aware**: the coefficient uses the live internal rate `fs` (so the notch positions are correct at 1×/2×/4×/8×).

### Parameters

- No new `_NT_parameter`s: fc's base is internal per-render state and its sweep is the P4 Contour destination; the existing **"P4 depth"** scales the sweep and **"P4 phaser"** (Processors page, Off/On) enables the channel. (The label was "P4 crush" before 2026-10-21.)

### Implementation status (built and validated 2026-10-21)

`p4_phaser.h` implements the cascade + dry/wet mix as above. `test_p4_phaser.cpp` verifies: exactly 6 notches (analytic) ⇒ 12 stages; the **measured** magnitude matches the analytic `|cos(6·φ₁)|` within 0.03–0.05 across nine probe frequencies; a notch is deeply attenuated (<0.12) while peaks pass at ~unity; **raising fc sweeps the comb upward** (the check that caught the coefficient-sign bug); static output obeys the sum-of-partials bound; a smoothly swept fc stays finite/bounded across 48k–384k; and the randomized base is seed-reproducible. ARM cross-compile clean.

## 6. Execution model: render vs. playback

The plugin's `step()` does two jobs of very different weight:

1. **Player (always-on, real-time):** reads from the *current kit* buffer and plays it back under control of the **Play** parameter (see "Player parameters" below). Comparable in cost to a basic sample-playback algorithm — this is the plugin's entire real-time obligation.
2. **Renderer (background, throttled):** advances the feedback-network computation (matrices, filters, processors, regulators) by one small bounded chunk per `step()` call — an explicit state machine, since the platform has no threads and `step()` must never block. Produces a fixed-length buffer at whatever pace the chunk size allows (render speed is a free design parameter, not fixed — e.g. ~4× slower than real-time is one possible operating point, not a requirement).

**Double buffering is required, not just the simplest option (locked 2026-10-05): playback must be gapless.** A *current kit* (playable) and a *next kit* (being rendered) are both kept fully resident. The renderer only ever writes into the inactive buffer; the pointer swap to make it the new *current kit* happens atomically, only once the render is fully complete. The player never sees a partially-rendered buffer.

This is close to the only sound approach for this plugin's **eventual** playback model, not merely the easy one. A kit is rendered as one continuous take and then **sliced and triggered randomly, likely polyphonically** — so there is no single predictable read position a leaner scheme (e.g. a rolling render that stays safely behind one read pointer, or double-buffering only the currently-changing segment) could rely on. Any slice can be hit at any moment, including rarely-used ones, including several overlapping at once, so the entire buffer has to be treated as potentially live for as long as it's the current kit. The only way to shrink this cost below full double buffering would be redesigning around independently-regenerable slices (each its own small buffer, refreshed on its own schedule) rather than one coherent continuous take chopped into related parts afterward — a different instrument, not a free optimization of this one.

### Player parameters (interim), locked 2026-10-18

The sliced/polyphonic player above is the target design (§10), but isn't built yet. For now, playback is deliberately minimal — a stand-in to validate the rest of the signal path before the real playback engine exists:

- **Play**: on/off (`kNT_unitEnum` {"Off","On"}). Play=1 loops the current kit buffer continuously from start to finish; Play=0 stops — output interpreted as silence (an assumption; confirm if a held-last-value behavior is wanted instead). Real `_NT_parameter`.
- **Pitch** (Render page, added 2026-10-20): ±48 semitones (±4 octaves), sets the finished kit's pitch by **playback speed** — the player's fractional read step is multiplied by `2^(semitones/12)`, so lower pitch reads the kit slower (longer & lower), higher pitch faster (shorter & higher). Purely player-side: read live, takes effect immediately with no re-bake. No pitch-tracking resampling or anti-aliasing — fast playback undersamples the 32kHz kit (some aliasing on the way up), consistent with the plugin's lo-fi-sampler character.
- **Output bus**: selects which bus the player writes rendered kit audio to. Real `_NT_parameter`. **Resolved 2026-10-19 (Step 15): uses `NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE`** — i.e. it DOES get the paired Output Mode (Add/Replace) parameter, following the API's own convention (every audio output in this project's reference examples uses `_WITH_MODE`). Replace overwrites the bus; Add sums the kit onto it.

The double-buffering requirement (above) is unaffected by this simplification either way — an atomic swap into a freshly-rendered kit is needed regardless of how the current one is being played back.

### Implementation status (Step 15, built and validated 2026-10-19): full plugin integration — first loadable/playable build

`4x4.cpp` is the first complete, loadable, playable build — the integration layer wiring the renderer (§7), kit buffer (§8), Contour destinations (§6), feedback network (§1), and all four processors into the `_NT_algorithm`/`_NT_factory` structure. (The Step 1 scaffold is archived at `History/4x4_v1.cpp`, per CLAUDE.md's versioning rule; this is v2.)

- **Memory placement**: the large `Engine` (renderer + double kit buffer) is placement-`new`'d into the firmware's DRAM block (`req.dram = sizeof(Engine)` = **514,056 bytes**, measured via the host harness — comfortably inside the 512KB/524,288-byte pool with ~10KB to spare); the small algorithm instance (48 bytes) lives in SRAM. A compile-time `static_assert` guards the DRAM budget so a future edit can't silently overflow it.
- **Parameters** — the authoritative current set (34 total, 7 pages) is the table in §9; it grew from the 24/6-page Step-15 state as the Depth page gained P3/P4, the Filters page became a bandpass (Freq+BW), and Oversample was added. **P1–P4 depth are 0-100% performer SCALES on each processor's Contour-driven modulation** (§2–§5: the parameter scales the curve's reach, the curve supplies the motion, the randomized base supplies the center — not a second independent value).
- **Processors on/off (added 2026-10-19 on user request)**: each of the 4 processors has an enum Off/On parameter on a "Processors" page. When a processor is **off, its channel is muted both in and out** — the channel receives no starter/feedback signal (`Σn = 0`), its processor is **skipped entirely** (CPU saved, which matters most for P1's FFT), and its output is forced to 0 so it contributes nothing to the final 4-channel mix *and* feeds 0 into its feedback-matrix column. Read live every `step()`, so it gates the render currently baking and every render after — but not an already-baked kit the player is looping (re-bake to hear a change, same as the other render-side controls). All four off → a silent render.
- **Regenerate (fixed 2026-10-19)**: a momentary trigger that was originally handled in `parameterChanged` with an auto-reset — which did **not reliably fire on the hardware** (first on-device test). Reworked to **edge-detect the 0→1 transition on the live `v[]` value inside `step()`** (the same "read v[] live" path everything else uses, so it no longer depends on `parameterChanged` firing at all; `parameterChanged` is now `NULL`), then auto-reset to Hold via `NT_setParameterFromAudio`. The edge re-arms once the reset lands, so repeated triggers work; holding at Regenerate doesn't spam renders.
- **Contour settings page (added 2026-10-19 on user request)**: the shared settings bank (Windows 1–32, Chaos/Round/Hold as 0–100%, Radius as 0–500ms) is now a real performer-facing parameter page — **superseding the interim "compiled-in constants" decision** (§9). Settings are **global (one bank applied to all Contour destinations, now 21) with per-instance seeding unchanged** (each destination still seeds from `baseSeed + index`), exactly as requested. They're read live every `step()` and stored as *pending*; the renderer applies them (reseed + rebuild of all engines — too heavy to do mid-render) only at the next render start, and only if they actually changed, so an unchanged bank still preserves Contour's carry-forward between renders (§6). Change a setting, hit Regenerate, hear the new character.
- **Player**: reads the current kit buffer with a fractional-position linear-interpolated loop (32kHz kit → the module's own sample rate), honoring Add/Replace mode; **Play=Off outputs silence** (§6's assumption, confirmed). One render chunk is advanced per `step()` call (§7's throttle). Continuous parameters are re-synced from live `v[]` every `step()` (not cached in `parameterChanged`), matching the monosynth example's robustness so preset loads take effect even if `parameterChanged` doesn't fire; `v[]` is deliberately never read in `construct()` (the renderer's internal defaults already match the parameter defaults, so a default load bakes correctly from its first kit).
- **Validated end-to-end on desktop** (`test_plugin_host.cpp` — a host harness stubbing the firmware API and driving the real factory entry points): `calculateRequirements` → allocate → `construct` → 20,000 `step()` calls. Confirms the DRAM request fits the pool; the player is silent until the first kit finishes baking then produces bounded (±11V), non-silent audio; Play=Off gives exact silence; Regenerate auto-resets. ARM cross-compile is clean (`make cpp SOURCE=4x4/4x4.cpp OUT=4x4/4x4.o`); static footprint is tiny (text 11KB, data 396B, bss 8.2KB — the big Engine is runtime-allocated, not static). **What remains is Step 16: the on-device reality check (load on hardware, profile CPU/memory, choose the final render-chunk size, listen) — the one step needing the physical module.**

Baked parameter-automation curves (pitch shift amount, feedback amplitude/polarity, HPF/LPF cutoffs, bit depth, sample-rate divisor, and others) are evaluated against the **renderer's own internal virtual sample-position counter**, not wall-clock time or `step()` call count — this keeps automation correct regardless of how fast or slow the render chunking happens to run.

### Modulation sources

> **ARCHITECTURE CHANGED 2026-10-21 — single contour + "playheads" (user request, "to make the plugin simpler").** The description below (one `contour::Engine` instance per destination, 21 of them) is the *prior* design. It is **superseded** by a single-contour playhead model; the paragraph is kept for context but the two paragraphs immediately following it describe the current system.
>
> **Current model:** exactly **one** contour curve is generated per render and baked into a small cyclic lookup table (`kTableSize = 1024` points spanning the whole render = 360°). Each of the **21 modulation destinations is a "playhead"**: a readout position = a fixed, seed-derived **phase offset in degrees** (stored as [0,1) = 0–360°) added to a single **base phase** that sweeps 0→360° across the render. So every head traverses the **entire slope once** (a full 360° pass over the kit's duration), but each starts at a different point, so the 21 destinations are decorrelated instead of moving in lockstep. Per internal sample the cost is now **one cheap table LERP per destination** — no per-destination envelope generation at all. (`contour_destinations.h`; validated in `test_contour_destinations.cpp`: all 16 crosspoint heads traverse the same value range over a pass = one shared curve + full pass; offsets decorrelated; seed-reproducible.)
>
> **Why:** the old 21-instance design was the dominant per-sample CPU cost of a bake, and because the bake runs inside the real-time `step()` callback (§7) that cost — spiking alongside P1's FFT — periodically pushed the module past its 90% CPU-mute threshold, muting outputs in bursts ("render bleed", heard as periodic cracking). Collapsing 21 envelope generators into 1 baked curve + 21 LERPs removed that cost and ~26 KB of state (`sizeof(ContourDestinations)` 31,672 B → 5,792 B). The crosspoint/P1/P2/P3/P4/filter-Q mapping ranges are unchanged.
>
> **Per-render randomisation (2026-10-21, user request: "the contour should also be randomised … a new seed each render").** `onNewRender()` takes a fresh per-render seed — **the same `baseSeed + renderIndex`** the processor bases use (§2–§5) — and re-derives BOTH the curve shape AND the 21 playhead phase offsets from it. So each render is a genuinely new modulation gesture (still shaped by the settings bank), not just new processor bases. It stays deterministic/reproducible for a given render index, and **costs nothing extra** — the single curve's table was already re-baked every render; only the seed now varies. (Earlier the curve used one fixed lifetime seed, so the gesture repeated every render; that is no longer the case.)

Motion for the feedback mixer's 16 crosspoint gains and the processors' own depth/offset parameters (P1/P2/P3/P4), plus **one shared filter-Q curve** used only when the QCONT toggle is on (§1 — it drives all 4 bandpass bandwidths; the filter *center* frequencies are never Contour-driven, they are either the static Freq params or VCFRND's per-render randoms), was generated — in the prior design — by embedded `contour::Engine` instances (Expert Sleepers' Contour generator, reused here as an internal engine rather than its own algorithm — see `CONTOUR_NT_SPEC.md` / `contour_engine.h`), one instance per destination. Each instance produced a seeded, deterministic one-shot/loopable envelope curve (DX7-style no-reset dual-stage envelopes, with optional hold-freeze regions and rounded corners). **Now a single such engine generates the one shared curve** that all playheads read.

The one contour shares the global settings bank — Windows, Chaos, Radius, Round prob, Hold prob — set once; **Length is not part of this performer-facing bank**, it's locked to equal the kit's own duration (§8). Under the current model these settings shape the single curve; the per-destination variety comes from the playhead phase offsets, not from per-destination seeds.

**Evaluation is lazy, not baked — committed 2026-10-18 (§8).** Rather than `rebuild()` precomputing and storing the entire render-length breakpoint curve up front, each envelope segment's parameters are computed on demand via Contour's keyed random lookup (`rnd(seed, i, j)`) the moment playback actually reaches that segment, keeping only the current segment's scalars plus one segment of lookahead (needed for the corner-rounding math, which blends using the *next* segment's slope). Only the hold/round region `Span` lists still need precomputing in `rebuild()`, since region placement is a genuine decision over the whole render timeline that can't be made lazily. This is a deliberate, explicit deviation from `CONTOUR_NT_SPEC.md`'s "use the reference engine as given," undertaken specifically to free DRAM for kit duration — see §8 for the memory numbers and the engineering-risk tradeoff.

Each instance's `process()` must be called exactly once per virtual render sample actually advanced within a chunk — not once per `step()` call — to keep its internal clock in lockstep with the renderer's own virtual sample-position counter. Calling it at the wrong rate desyncs the modulation from the audio being rendered.

At the start of each new kit generation, instances are **re-triggered, not reconstructed** (`trigger()`, not placement-new): Contour's envelopes carry their current level forward rather than resetting, so successive kit generations transition smoothly instead of restarting cold. `rebuild()` (needed after any settings change, and now scoped to just the region-selection draws rather than every envelope's parameters) is cheap for one instance, but if many instances need rebuilding at once (e.g. all at a new kit's start), that work should still be staggered across several render chunks rather than done in a single call, following the same chunking discipline as the FFT and matrix/filter passes.

Contour's native output is linear volts (0…Level, or ±Level + Offset). That maps directly onto destinations like crosspoint gain via Level/Offset/Bipolar, but destinations with a different native scale — filter cutoff in Hz, for instance — need an additional mapping layer (likely logarithmic/exponential) that Contour doesn't provide on its own.

### Implementation status (Step 12, built and validated 2026-10-18): instance management and destination wiring complete

> **Superseded 2026-10-20:** the destination set is now **21** — the 8 filter cutoffs left (static bandpass) and P3's fold+bias became one delay-time destination. The Step-12 description below is the historical record at that step.

`contour_destinations.h` implements one `contourlazy::Engine` per real destination, enumerated from every processor and filter built in Steps 4/5/7/9/11 — **30 total at Step 12**: 16 feedback crosspoints, 4 HPF cutoffs + 4 LPF cutoffs, P1 depth, P2 depth, P3 fold, P3 bias, P4 bit-depth, P4 rate-divisor. All 30 share one `SharedSettings` bank (windows/chaos/radius/round-prob/hold-prob) plus the project's established ±5V bipolar output convention (levelV=5, bipolar=true) — only each instance's seed (`baseSeed + destinationIndex`) differs, exactly per §6's pattern. Length is excluded from the shared bank and passed in separately, locked to the kit's own duration as already established (§8).

- **Output-unit mapping layer — designed and implemented, resolving the gap this section flagged.** The doc mandated the *technique* (direct for gain-like destinations, logarithmic/exponential for Hz-type ones) but left the specific numbers open; these are this step's own documented design choices, not pre-specified:
  - **Crosspoint gain** (16): direct linear, ±5V → ±1.
  - **HPF/LPF cutoffs** (8): exponential, ±4 octaves across the full ±5V swing, centered at 100Hz (HPF — mostly removes rumble/sub content) and 2000Hz (LPF — general band-limiting).
  - **P1/P2 depth** (2): direct linear onto a *fraction* of each processor's total range — ±12 semitones for P1, ±500Hz for P2 — since the randomized base (§2/§3) can already span the full range on its own.
  - **P3 fold/bias** (2): reuses the `foldFromCurve`/`biasFromCurve` helpers already implemented in `p3_wavefolder.h` (Step 9) — no new mapping needed.
  - **P4 bits/divisor** (2): direct linear onto each integer range (1-16, 1-64).
- **Usage discipline enforced by the API shape, not just a comment**: `contourlazy::Engine::process()` both advances its internal clock and returns the current value in the same call (no separate peek accessor exists), so `ContourDestinations::advance()` calls each of the 30 exactly once per invocation and caches the results — the mapping accessors read the cache, never calling `process()` a second time, which would silently desync an instance's clock from the others (§6's stated risk).
- **Wiring glue**: `fillChannelControls()` and `fillMatrix()` populate a real `feedback_network.h` `ChannelControls`/`FeedbackNetwork` directly from the current cached state. The actual per-sample driving loop (`advance()` → fill → `FeedbackNetwork::processSample()`) belongs to the renderer (Step 14), which owns both objects together; this step provides the wiring, not the scheduling.
- **Validated on desktop** (`test_contour_destinations.cpp`): all 16 crosspoint instances produce distinct values (confirms seeds aren't accidentally colliding); every mapped destination stays within its documented native range across a full kit-length run; output stays bounded immediately after a re-trigger; the wiring glue drives a real `FeedbackNetwork` for a full second with no NaN. ARM cross-compile is clean; **`sizeof` for all 30 engines plus their cached state measures exactly 45,240 bytes on both x86 and ARM** (44.18KB) — matching the per-instance 1,504B figure × 30 (§8) to within struct padding. (Note: Step 14's whole-system ARM measurement, not this per-component figure, is what ultimately fixed the kit duration — see §8.)

## 7. CPU budget strategy

Because the renderer has no deadline (the player just keeps using whichever kit is current until a new one swaps in), render CPU cost is a tunable design parameter rather than an emergent property of the algorithm:

- **Always-on cost:** player only — small, constant, every `step()` call.
- **Render cost:** incurred only while actively baking a new kit; chunked into small bounded units (e.g. one FFT butterfly stage, or one stage of the matrix/filter/regulator pass) sized so that render-chunk cost + player cost + headroom for other algorithms in the same preset stays well under the module's CPU-overload threshold on *every single call* — average cost over a render cycle is not the relevant number, peak per-call cost is.
- Smaller chunks → lower peak CPU, longer bake time. Larger chunks → faster bake, higher (but still bounded and chosen) peak cost. This trade is free to make because nothing downstream has a deadline.
- Each embedded Contour instance's `process()` call (now also computing the current segment's parameters on-demand when crossing into a new one, under the lazy evaluation model, §6) and its occasional `rebuild()` (now scoped to region-selection draws only, likely cheaper than the baked engine's ~700-draw figure — not yet quantified) are part of the same per-chunk budget as the FFT/matrix/filter work — they don't get a separate allowance.
- Reference point: the LASER example plugin runs 20–30% CPU as a genuine real-time algorithm with a forced per-call cost. This design's forced per-call cost (the player) should be much lower, since the expensive part is optional and schedulable rather than forced every call — the actual number can only come from on-device profiling.

> **"Render bleed" — observed and fixed 2026-10-21.** The theory above held that render cost is "schedulable," but in practice the bake runs **inside the real-time `step()` callback** (chunked, `kChunkSize` internal samples per call), so its peak-per-call cost competes directly with live playback for the module's CPU budget. The old per-sample cost (21 Contour engine `process()` calls + P1's periodic FFT, ×`kChunkSize`) periodically exceeded the **90% CPU-overload threshold → the module muted all outputs** in bursts, heard as periodic cracking in the playing kit whenever a render ran. **Primary fix:** the single-contour "playhead" redesign (§6) removed 20 of the 21 per-sample envelope generators — the dominant cost — giving large per-call headroom (DRAM also dropped ~26 KB). **Secondary knob:** `kChunkSize` (renderer.h) is kept at 16 (≈6.6 s first bake at 1×) but is the documented on-device fallback — lowering it reduces peak per-call cost proportionally at the cost of a longer bake (free per the no-deadline argument; the old kit keeps playing until the new one swaps in). P1's FFT remains the one unavoidable periodic spike; with the contour cost gone it should sit under the threshold, to be confirmed on-device (Step 16).

### Implementation status (Step 14, built and validated 2026-10-19): renderer state machine complete

`renderer.h` implements the throttled, chunked, resumable render loop tying together the feedback network (Steps 10-11), Contour destinations (Step 12), and kit buffer (Step 13). `advanceChunk()` advances up to `kChunkSize` internal render samples per call (intended once per `step()`), maintaining its own virtual sample-position counter; it's a no-op when idle and returns `true` on exactly the call that completes a render (after the atomic buffer swap).

- **Chunk size**: `kChunkSize = 16` render samples/call — picked conservatively since no on-device profiling exists yet (Step 16) to measure the real peak cost of one internal render sample (30 Contour advances + the full 4-channel HPF/LPF/Pn/Reg chain + matrix mix). A single named constant, trivially tunable once profiling data exists, exactly as §7 frames the trade.
- **State continuity between renders — resolved here (§10 had left it open).** Processor/filter/Reg internal state (P1's phase-vocoder buffers, P2's allpass history, P4's hold, HPF/LPF histories, Reg's DC-block/AGC envelope, the matrix's one-sample delay) **all reset cold** at each new render via `FeedbackNetwork::onNewRender()`. Only Contour carries anything forward (its envelope *level*, via `trigger()`'s no-reset behavior, §6). Reasoning: the whole design rests on seed-reproducibility (every random element is seed-derived) — if a render's DSP state depended on leftovers from whatever render ran before, the same seed would stop producing the same kit, for no benefit. The starter wave (§1) exists precisely to kick-start a cold network each render; carrying old processor state forward would undercut that. Contour's `trigger()` is the sole exception because a level-reset there would be an audible jump with no upside, whereas processor plumbing state has no "character worth preserving."
- **Contour seeding across renders**: instances keep the SAME seed for the plugin's lifetime (assigned once in `initialise()`) and are only ever re-triggered, never reseeded, on an ordinary new-render trigger — reseeding would force a full `rebuild()` of all 30 every render (the staggering cost §6 warns about) and a bigger creative jump than "smooth transition" implies. P1/P2's OWN randomized base (§2/§3, separate from Contour) still gets a genuinely fresh draw each render via an incrementing render index folded into the per-render seed passed to `FeedbackNetwork::onNewRender()`.
- **Render-rate → kit-rate conversion**: the network runs at the render sample rate (`NT_globals.sampleRate`, checked at runtime, never hardcoded), the kit is fixed 32kHz (§8); the ratio isn't generally an integer (48000/32000 = 1.5, but other rates give non-integer ratios), so a fractional accumulator emits a kit sample whenever it crosses the next kit-sample boundary. No anti-aliasing on the decimation — consistent with the project's accepted-aliasing stance (§4/§5). Verified to produce exactly `kNumSamples` kit samples at both 48kHz and 44.1kHz (non-integer ratio 1.378125).
- **4-channel → mono mixdown**: not specified in the locked design (checked) — this step's own decision. Sums all 4 "Out n" taps (standard N-signals-onto-one-bus) with a 0.5× (6dB) attenuation to keep ordinary correlated sums inside the kit buffer's ±11V clamp rather than leaning on the clamp. Measured: only ~0.012% of a real rendered kit's samples actually reach the clamp (rare transient peaks) — consistent with the project's "artifacts are character, not flaws" stance rather than something to further tame.
- **Validated on desktop** (`test_renderer.cpp`): a full render completes in the expected chunk count and produces bounded (±11V), non-silent audio; two consecutive renders both complete cleanly with no divergence carried across (confirming the cold-reset decision works); the idle no-op path is safe. **ARM cross-compile fits the budget** — the whole assembled `Renderer` measures 522,240 bytes bss, 2KB under the 512KB DRAM pool (this is the measurement that drove the kit-size correction in §8).

### Oversampling (added 2026-10-19 on user request)

The renderer runs the **entire network at `oversample × NT_globals.sampleRate`** internally (factor 1/2/4/8 → 48/96/192/384 kHz on a 48 kHz module), then passes the mono mixdown through an anti-aliasing low-pass (`filters::AntiAliasLowpass`, a 4-pole cascade at ~13 kHz, just under the 32 kHz kit Nyquist) **before** the decimator samples it. This is exposed as a real `Oversample` parameter (enum 1x/2x/4x/8x, default 1×) on the Render page, applied at the next render start (the rate can't change mid-bake without corrupting the decimation accumulator).

- **Why this is cheap here, uniquely:** the bake is offline, so a higher factor only lengthens the bake (~linearly: ~7 s at 1× → ~28 s at 4×) and **does not raise peak CPU per `step()`** — each `step()` still advances the same bounded chunk, there are just more of them. So oversampling can never cause a module overload, only a slower bake. It's also memory-free (the network is sample-by-sample; the AA filter is a handful of floats on the mono stream).
- **The AA downsample is foundational and runs at every factor, including 1×** — previously the 48→32 kit decimation just dropped samples with no filter, folding 16–24 kHz into every kit. Even 1× now gets that cleaned up (slightly less top-octave hash than the first on-device build).
- **Uniform, whole-network, P4 included** (per the user's choice): the feedback loop is a single per-sample recursion, so the whole loop runs at one rate — there's no way to oversample one processor in isolation without resampling inside the loop.
- **Per-processor tradeoffs, honestly:** P3 (wavefolder) is the clean win — measured ~18× less in-band alias energy at 4× (`test_oversampling.cpp`). P4 (bitcrusher) gets cleaner/subtler. But P1 (FFT) gets *coarser* frequency resolution at high factors (fixed 1024-pt/256-hop over a wider band → weaker pitch tracking at 8×), and P2 (Hilbert) loses low-frequency phase accuracy (its 48 kHz-derived coefficients hold ~90° only down to ~100 Hz at 4×). So **2×–4× is the recommended sweet spot**; 8× suits P3-dominant/clean patches. Different factors also produce **genuinely different kits** (the internal rate changes the loop's one-sample-delay timing), not just cleaner versions of the same kit — raising the factor is a sound change, not only a quality toggle. A future refinement could store per-factor P2 coefficient sets and/or a steeper polyphase AA filter.
- **Validated**: `test_oversampling.cpp` (deterministic 18× alias-energy reduction on P3 at 4×, plus the AA filter's high-freq attenuation), `test_filters.cpp` (AA filter response), and `test_plugin_host.cpp` (a full 4× render runs end-to-end, bounded/non-silent/NaN-free). ARM-compiles clean, still within budget.

## 8. Memory

All allocation is static, sized via `calculateRequirements()` — no `new`/`malloc` at runtime anywhere, per the platform's hard constraint.

**Confirmed budget (2026-10-18): DTC 24KB, ITC 64KB, DRAM 512KB — no separate SRAM allocation is being counted on.** These are shared totals across every algorithm loaded in the same preset (the module runs up to 32 simultaneously), not a guarantee reserved for this plugin alone.

### Contour per-instance size: baked vs. lazy

The original reference engine *bakes* — `rebuild()` precomputes and stores the entire render-length curve as breakpoint arrays. Deriving that engine's size directly from `contour_engine.h`'s array declarations gives, as a function of `kMaxWin` (W) and `kPieces` (P): `size(W,P) ≈ 1328·W + 160·P + 928` bytes — ≈47.3KB at shipped defaults (W=32, P=24), ≈13.2KB at a trimmed W=8/P=12. Both figures are correct; they're just different trim levels of the same formula, not a contradiction.

**Superseded 2026-10-18: committed to a lazy/on-demand reimplementation instead of the baked engine, specifically to free DRAM for kit duration.** This is a genuine rewrite of `rebuild()`/`process()`, not a settings change, and is an explicit, deliberate deviation from `CONTOUR_NT_SPEC.md`'s "use the reference engine as given" — a real implementation and testing effort.

- **Why it's structurally sound**: Contour's randomness is a *keyed lookup* (`rnd(seed, i, j)`), not a sequential stream — `CONTOUR_NT_SPEC.md` states independent draws never disturb each other. That means any envelope segment's parameters can be computed on the spot, the moment playback reaches it, without needing the whole curve precomputed first.
- **What still has to be precomputed eagerly**: the hold/round region `Span` lists (`probWindows()`) are a genuine global decision over the whole render timeline — they can't be made lazily. This is the one part of the engine that still scales with a trim parameter (`kPieces`), and it's cheap: 32 bytes/piece.
- **What becomes lazy**: the envelope/window breakpoint curve. Only the *current* segment's scalars, plus *one segment of lookahead* (the corner-rounding math needs the next segment's slope to blend correctly near a boundary), are kept in memory — a handful of doubles, regardless of how many total windows/envelopes the render contains.
- **Consequence: `kMaxWin` no longer needs trimming for memory reasons at all.** Window count only affects the lazy scalars' *bookkeeping*, not their *size* — the per-instance footprint is independent of how many windows exist in the render. `kMaxWin` can return to its native ceiling (32, matching the "Windows" parameter's own documented range) with no memory cost. This fully resolves the earlier caveat about the W=4 trim capping Windows below its default of 6.
- **Measured per-instance size (Implementation Plan Step 3, 2026-10-18): `sizeof(contourlazy::Engine) = 1504 bytes`** — a resident breakpoint window of 12 `Pt` entries (48 bytes each) plus the `hold_`/`round_` span arrays at native `kPieces`=24 (32 bytes/piece) plus scalar state, confirmed via `arm-none-eabi-c++` compilation and validated against the reference engine with 2,200+ randomized configurations (golden tests + dense sample-by-sample diffs + a 2,000-run randomized sweep), all exact to floating-point precision. This is **≈31× smaller** than the baked engine's 47.3KB at the same native settings — a real, measured result, not the ≈1.0KB pre-implementation guess (the window capacity needed to be 12, not the ~3 minimum theoretically sufficient, to safely cover worst-case hold/rounding interactions without starving the generator's lookahead).
- **Secondary benefit**: `rebuild()`'s cost likely drops too, since most of the ~700 random draws in the original engine were for precomputing every envelope's parameters up front — the lazy version defers almost all of that to whenever each segment is actually reached, leaving only the region-selection draws in `rebuild()` itself. Not yet quantified.

### Locked kit-buffer / Contour configuration — recomputed 2026-10-21 (single contour, 21 playheads, + P3 delay line)

**Current destination count: 21 — now 21 playheads on ONE shared contour curve** (not 21 engines; §6, redesigned 2026-10-21): 16 feedback crosspoints + P1 depth + P2 depth + P3 delay + **P4 phaser center-freq sweep** + **1 shared filter-Q mod**. History of the count: peaked at 30, dropped to 21 (2026-10-20) when the **8 filter cutoffs stopped being Contour destinations** (static bandpasses now, −8) and the **P3 wavefolder's fold+bias (2) became a single P3 delay-time destination (1)** (−1 net), rose to 22 when QCONT added a shared filter-Q curve (+1), then back to **21 (2026-10-21)** when P4's bitcrusher (2 destinations) became a 12-stage phaser (1 destination). Independently, the 2026-10-21 single-contour redesign changed *how* these 21 are produced — one baked cyclic curve read at 21 seed-derived phase offsets — which is what collapsed the Contour memory from ~31.6 KB to ~5.8 KB (below). A cost alongside the delay: the **P3 delay ring buffer, 19,200 B** (100 ms @48 kHz base), in DRAM. **Note — filter *cutoffs* are never Contour destinations;** VCFRND randomizes them once per render.

**Kit size — trimmed 2026-10-20 to fix a DRAM-overcommit regression.** The budget must cover BOTH
the req.dram `Engine` AND the plugin's own static `.bss` (~8,196 B, mostly fft.h's `g_twiddle`),
which the firmware loader also places in RAM. Earlier sizing only checked `sizeof(Engine)` and
ignored the static footprint; once the P3 delay buffer (+19 KB) landed, the real total became
**bss 8,196 B + Engine 519,936 B = 528,132 B — ~3.8 KB OVER the 512 KB (524,288 B) pool** (the
last build that fit, "step 15", was 522,292 B total and worked on hardware). This is the most
likely cause of the plugin going silent on the module (desktop has separate bss/heap + abundant
RAM, so it never showed). **Fix: kit trimmed to `kNumSamples = 141000` (≈4.41 s), Engine now
501,328 B; with the 8,196 B static bss the real total is 509,524 B — ~14.4 KB under the pool** (and
12.5 KB below the known-good step-15 build). The `static_assert` now reserves 16 KB
(`sizeof(Engine) ≤ 512 KB − 16384`) so it covers the static footprint too and this can't recur.
**(Update 2026-10-21a:** QCONT added one shared Q-mod Contour (+1,520 B); the P4 bitcrusher→phaser swap
removed one Contour destination (−1,504 B). Engine was briefly **501,432 B**.)
**(Update 2026-10-21b — single-contour playhead redesign, §6):** replacing the 21 per-destination
Contour engines with ONE engine + a 1,024-point baked curve + 21 phase offsets dropped
`sizeof(ContourDestinations)` from ~31,672 B to **5,792 B** (−~26 KB). **Engine is now 475,552 B; with
the 8,196 B static bss the real total is 483,748 B — ~40.5 KB under the 512 KB pool** (static_assert
slack ~32 KB). This headroom is the most comfortable the project has had.

| Item | Approx. size | Notes |
|---|---|---|
| Kit buffer, double-buffered — **12-bit packed, 32kHz, mono, ≈4.41s/buffer (141,000 samples)** | ≈206.5KB/buffer, ≈413KB total | Trimmed 2026-10-20 from 147,204 (4.60s) to fix the bss+Engine DRAM overcommit (see paragraph above). |
| P3 delay ring buffer | 19,200 B (4,800 floats) | 100 ms @48 kHz base; max delay scales down with oversampling (§4). New 2026-10-20 (replaced the stateless wavefolder). |
| FFT/pitch-shifter scratch (in-place 1024-pt complex buffer, overlap-add tail, window) | ~20–25KB | Inside P1, in the `FeedbackNetwork`. |
| Matrix / per-channel filter / regulator / P4 state | negligible, ~1–2KB | Per-channel resonant bandpass biquad (9 floats each), Reg, P2 allpass, P4 phaser (12 allpass pairs + base freq). |
| Contour — single curve + 21 playheads (2026-10-21, §6) | **5,792 B total** (1 engine 1,504 B + 1,024-pt curve table 4,096 B + 21 offsets/values) | Replaced 21 per-destination engines (~31.6 KB). One baked cyclic curve read at 21 seed-derived phase offsets. Measured via `sizeof()`. |
| Time-stretch period maps + analyzer (2026-10-22, §11) | **~17.9 KB** (2 × PeriodMaps 8,278 B + Analyzer 1,312 B) | Per-buffer period/confidence maps (packed uint16+uint8, 2759 entries), swapped with the kit; built post-render. |
| Baked automation curve tables | n/a | Resolved in favor of inline, lazy `process()` evaluation against the render's virtual sample position (§6) rather than pre-computed stored tables. |

### Implementation status (Step 13, built and validated 2026-10-18): kit buffer and double buffering complete

`kit_buffer.h` implements the storage layer with an exact (not approximate) sample count. History: 151,188 (per-category estimate) → 147,204 (Step 14, whole-system ARM measurement) → **`kNumSamples = 141000` (2026-10-20)**, trimmed once it was found that the earlier budgeting ignored the ~8 KB static `.bss` and the delay buffer had pushed bss+Engine over the 512 KB pool (the silence-on-hardware regression). Even, clean pairing; 211,500 bytes/buffer; **`kDurationSec = 4.40625s`** — the authoritative kit duration.

- **12-bit packing**: standard two-samples-per-3-bytes scheme (`byte0=a[11:4]`, `byte1=(a[3:0]<<4)|b[11:8]`, `byte2=b[7:0]`), signed, full-scale ±11V (the hardware's absolute output ceiling, already guaranteed by Reg's soft clip, §1). Quantization step ≈0.00537V; measured max round-trip error on a float32 sine sweep is ≈0.00269V — within one step, confirming correct round-to-nearest behavior.
- **Write-order independence confirmed, not just assumed**: each `write()` call reads back and preserves whatever currently occupies its packed group's other slot, so writing a pair's samples in either order produces byte-identical results — verified directly (`test_kit_buffer.cpp`) by writing an entire buffer odd-before-even and comparing against the normal even-before-odd order.
- **Double buffering**: a single `activeIndex_` int flip (`swap()`) is the only state that changes to make a freshly-rendered buffer current — in this platform's single-threaded, non-preemptive `step()` model (no OS, no threads, per CLAUDE.md), that one-instruction flip is indivisible from the player's perspective, satisfying "the player must never observe a partially-rendered buffer" without needing real atomics. Verified: writes to the inactive buffer are invisible to `current()` before `swap()`; immediately visible after; a second render's writes don't disturb the buffer the player is still reading from.
- **ARM-measured** (at the 141,000-sample size): both buffers + swap index ≈ 423 KB, the dominant term in the Engine. The single-contour redesign (2026-10-21) brought the Engine down to 475,568 B; the **time-stretch period maps** (2026-10-22, §11) then added 2 × 8,278 B + analyzer ≈ +17.9 KB. **Current Engine = 493,440 B; with the ~8 KB static bss the real RAM total is 501,636 B, ~22 KB under the 512 KB pool** (static_assert slack ~14 KB). The two still-pending features (pitch-shifter spectral maps, noise-mask analysis) will eat most of what's left — expect a small kit trim then.

## 9. Parameters vs. internal state

Only values the performer needs to control — via pot, encoder, CV, MIDI, or
I2C, visible on a GUI parameter page — need to be real `_NT_parameter`
entries routed through `self->v[]`. A plugin's parameter array is fixed-size
and finite (`req.numParameters = ARRAY_SIZE(parameters)`), so it should be
reserved for what's actually useful to a performer.

Everything else in this design is ordinary instance-struct data, with no
involvement from the parameter system at all:

- The feedback mixer's 16 crosspoint gains
- The per-channel resonant bandpass filter state (biquad histories). **Their Freq/BW ARE now real parameters** on the Filters page (§1), but the filter *state* is internal.
- P1's randomized base pitch-shift value (fresh each render, not performer-set — §2)
- P2's allpass filter states (coefficients resolved, §3), and its randomized base frequency-shift value (fresh each render, not performer-set, same treatment as P1 — §3)
- P3's delay ring buffer + randomized base delay time (fresh each render, not performer-set — §4); the delay-time Contour modulation is scaled by the real P3 depth param
- P4's current bit-depth and sample-rate-divisor values (curve-only, no base parameter — §5)
- Each Contour instance's generated curve state (breakpoints, playback position) and its derived seed
- The renderer's virtual sample-position counter
- Kit-buffer pointers and double-buffer swap state

These are declared as plain fields/arrays on the algorithm struct (sized via
`sram`/`dram`/`dtc` in `calculateRequirements()`, same as any other instance
data), initialized in the constructor, and read/written directly by the
render state machine. They never appear on a parameter page, are never
CV/MIDI-mappable, and don't count against the plugin's parameter budget —
because the user has no use for them.

**If any of this internal state must survive a preset save/reload** —
e.g. a baked curve authored per-preset rather than fixed at compile time —
that's still not a job for `_NT_parameter`/`v[]` (which only covers what's
declared in `parameters[]`). The correct mechanism is the factory's
`serialise`/`deserialise` callbacks (`serialisation.h`): a separate
persistence path that reads/writes arbitrary state directly to the preset's
JSON, fully decoupled from the parameter system. If the curves are fixed
constants compiled into the plugin, no persistence is needed at all — they're
just code.

What *does* likely need to be a real parameter — performer-facing controls
such as triggering a new render, selecting/triggering a kit slot, or a
macro like "intensity" — is not yet finalized; the principle above
(parameter only if the performer touches it) is the decided part, the
specific control list is still open. **Play and Output bus are now
confirmed, locked real parameters (§6)** — overall output level, as a
separate control, remains unresolved/open. P1's depth (§2) is a real
parameter; P1's base is not — it's randomized at each render, internal
state (§2). **P4 is now a 12-stage phaser (2026-10-21, §5)** with the
base+depth treatment: its base center frequency is randomized per render
(internal state), its sweep depth is the real "P4 depth" parameter — the
bitcrusher's two curve-only destinations are gone. **P2 settled on
base+depth, same as P1 (§3, Step 7)**: its depth is a real parameter, its
base is randomized at each render, never performer-set. **P3 settled on
curve-only (§4, Step 9)**: fold and bias were both internal state, no base
value — this was "same as P4" under the old bitcrusher design; P4 has since
moved to base+depth, so the P3 analogy is now historical.

**One exception worth flagging:** the Contour modulation engines' shared
settings bank (Windows, Chaos, Radius, Round prob, Hold prob, and the
rest — §6; Length is excluded, locked to the kit's own duration rather
than performer-set) is a small, fixed-size set of global controls, set
once, applied to every instance. Given the principle above, these read naturally
as real, performer-facing parameters — a global "character" page governing
all the generative motion in the patch — rather than internal state.
**Resolved 2026-10-19: a real performer-facing "Contour" parameter page** (5 params: Windows, Chaos, Radius, Round prob, Hold prob). Initially (Step 15) these were compiled-in constants to keep the first build minimal, but the first on-device test prompted promoting them to real parameters on user request — the mid-render-reconfiguration concern is handled by applying a changed bank only at the next render start (not live mid-bake), and only when it actually differs, so Contour's carry-forward is preserved for unchanged settings. The bank stays **global (one set applied to all Contour destinations — 21 as of 2026-10-20) with per-instance seeding** (`baseSeed + index`) unchanged. Everything else they drive (per-instance curves, seeds, playback state) stays internal.

**Final real-parameter set, 2026-10-20 (35 total across 7 pages: Starter / Depth / Render / Contour / Filters / Processors / Routing):**

| Parameter | Count | Notes |
|---|---|---|
| Starter frequency (S1–S4) | 4 | 20–2000Hz, static (§1) |
| Starter duration (S1–S4) | 4 | **1–500 ms** (min lowered to 1 ms, 2026-10-19), static (§1) |
| P1 / P2 / P3 / P4 depth | 4 | 0–100% scale on each processor's Contour-driven modulation; base is randomized internal state. P3 scales delay-time mod, P4 scales the phaser center-freq sweep (§2–§5) |
| Regenerate | 1 | Momentary enum trigger, edge-detected in step() (auto-resets); kicks off a fresh kit bake (§6) |
| Play | 1 | Loops the current kit on/off; Off = silence (§6) |
| Oversample (1x/2x/4x/8x) | 1 | Internal oversampling for less aliasing; applied at next render; offline so only bake time scales (§7) |
| Pitch | 1 | ±48 semitones (±4 octaves). Player-side: sets playback speed of the finished kit (lower = slower = longer & lower). Read live — takes effect immediately, no re-bake (§6). |
| Contour settings (Windows, Chaos, Radius, Round prob, Hold prob) | 5 | Global bank, per-instance seeds; applied at next render (§6/§9) |
| Filters: bandpass Freq (×4) + Bandwidth (×4) | 8 | Per-channel resonant bandpass before each Pn; Freq 20–16000 Hz, BW 1–96 semitones; **static, not Contour-modulated by default** (§1). Replaced the former HPF/LPF cutoffs 2026-10-20. |
| Filters: VCFRND + QCONT (Off/On) | 2 | VCFRND = randomize bandpass center freqs every render (overrides Freq); QCONT = Contour-modulate bandpass Q/bandwidth (overrides BW). Both default Off. Added 2026-10-21 (§1). |
| Processor on/off (P1, P2, P3, P4) | 4 | Off = channel muted in & out, processor skipped; render-side (§1/§6) |
| Output bus + Output mode | 2 | `NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE` — Output Mode pairing resolved YES (§6) |
| Norm level | 1 | 0–11 V. The baked kit is **peak-normalized to this level at playback** (Render page). Read live = a live output-level knob; since the player interpolates linearly (no overshoot) the output peak ≤ level, so any level ≤ 11 V is clipping-free. Added 2026-10-21 (user request). |
| **Stretch page**: Scale, Stretch, Threshold, Xfade | 4 | Period-synchronous, read-time time stretch of playback (§11). Scale (0–100 %, which repetition length to target across 10 octave scales 2 ms–1024 ms); Stretch (1.00×–8.00× = **finite** plays per slice — never stuck); Threshold (0–100 %, min periodicity to stretch); Xfade (1–50 ms splice crossfade). **Stretch=1.00× is bit-identical passthrough.** Added 2026-10-22 (user request). |
| **Shifter page** (P1): BlockSize, Swaps, SwapSize, Omit | 4 | Spectral manipulation of the **full-spectrum P1 pitch shifter** (§12). BlockSize (enum 1/2/4/8/16/32/64 bins/block, def 8); Swaps (0–100 % of block-pairs that exchange); SwapSize (0–100 % of max circular swap distance); Omit (0–100 % of blocks muted before the shift). Latched at render start; maps seeded from the per-render seed. **Swaps=0 % & Omit=0 % = clean full-spectrum shift.** Added 2026-10-22 (user request). |

(Total **46** parameters. Array tail: …Norm level(37), Scale(38), Stretch(39), Threshold(40), Xfade(41), BlockSize(42), Swaps(43), SwapSize(44), Omit(45) — each appended as added. Page membership is independent of array order: Scale/Stretch/Threshold/Xfade on the Stretch page, BlockSize/Swaps/SwapSize/Omit on the Shifter page.)

> **Parameter ordering rule (hard-won, §10):** the firmware restores a saved preset's values BY INDEX, so **new parameters must be APPENDED at the end** of `parameters[]` — never inserted mid-list. Inserting Pitch mid-list once shifted Output's index and remapped a stale value onto it (Output → Input 1 + render noise). VCFRND/QCONT were appended after Pitch (indices 35, 36) for this reason; they still *display* on the Filters page (page membership is independent of array order).

Internal state that is NOT a parameter: the 16 crosspoint gains and the processors' depth modulation are Contour-driven (21 destinations incl. the P4 phaser center-freq sweep and the shared filter-Q mod, §6); P1/P2/P3/P4 randomized bases (P4's is its phaser base center freq) are internal per-render state (§2–§5).

## 10. Open design questions

**Steps 1-15 complete; the plugin loads and plays on hardware, and has been through two rounds of on-device-driven changes.** Current state: all four processors — **P1** pitch shift, **P2** frequency shift, **P3 now a plain delay** (wavefolder omitted), **P4 now a 12-stage allpass phaser** (bitcrusher omitted 2026-10-21); the full per-channel chain (matrix, injection, starter wave, **per-channel resonant bandpass**, Reg — §1); the Contour layer — **now a single curve read by 21 "playhead" phase offsets, re-seeded per render** for a fresh modulation gesture each bake (§6, redesigned 2026-10-21 for simplicity + CPU/memory); the 12-bit double-buffered kit store with **loop-seam edge fades** and a **mix soft-clip** (§8/§1, 2026-10-21 pops fix); the chunked renderer with an **Oversample** control (§7); and the full `_NT_algorithm`/`_NT_factory` integration with **42 real parameters across 8 pages** (§9, incl. a player-side Pitch control ±4 octaves, the VCFRND/QCONT filter-modulation toggles, a **Norm level** 0–11 V peak-normalize/output-level control, and a **Stretch** page — time-stretch, §11). Validated end-to-end on a desktop host harness (**18 desktop suites green**) and ARM-compiles clean — **total RAM = 8.2 KB static bss + 475,552 B Engine = 483,748 B, ~40.5 KB under the 512 KB pool** (the single-contour redesign freed ~26 KB). **Changes driven by hardware testing (2026-10-19/20/21):** Regenerate reworked to reliable step()-edge-detection; Contour settings and a Processors on/off page added; an Oversample control; starter min → 1 ms; P3 wavefolder → delay; P1–P4 all given depth controls; the one-pole HPF+LPF (too gentle) replaced with a per-channel **resonant bandpass** (~3× more in-band energy); a player-side **Pitch** control (±4 oct via playback speed); a **silence regression fixed** (the kit was trimmed 4.60 s → 4.41 s because static bss + Engine had overrun the 512 KB pool by ~3.8 KB — see §8); a **misrouting regression fixed** (Pitch had been inserted mid-list, shifting Output's index — now all new params append at the end, §9); **VCFRND/QCONT** added (2026-10-21) — randomize bandpass cutoffs per render / Contour-modulate bandpass Q, both default Off and reproducible/stable (`test_filter_mod.cpp`); the **P4 bitcrusher replaced by a 12-stage allpass phaser** (2026-10-21, user request) — Contour-swept center frequency, no feedback/LFO of its own (`test_p4_phaser.cpp`); and a **2026-10-21 round fixing playback pops + "render bleed"**: a **mix soft-clip** (kills the kit's ±11 V hard-clamp pops), **loop-seam edge fades** (click-free loop), and the **single-contour "playhead" redesign** (one curve + 21 phase-offset playheads) which both simplifies the plugin and removes the per-sample Contour CPU that was tripping the 90% mute threshold during a bake; and a **Norm level** parameter (0–11 V) that **peak-normalizes the kit at playback** — a live output-level knob that also guarantees clipping-free headroom for any level ≤ 11 V (addresses the remaining quieter clicks the user attributed to clipping). Remaining: Step 16's deeper on-device profiling (real CPU%, chunk-size, listening) — in particular confirming the render bleed is gone, the loop is click-free, and the Norm-level headroom cured the quieter clicks.

**Workflow change + new feature wave (2026-10-22):** the user switched to **hardware-only testing** — from here the deliverables are the plugin `.cpp` and compiled `.o` only (no desktop test harnesses), and each feature is verified by listening on the module. Three features were specced (`period-stretch.md`, `pitch-shifter-swap-omit.md`, `noise-mask-prompt.md`), to be built in order: **(1) time stretch — DONE (§11)** (confirmed working on hardware 2026-10-22; the finite-repeat fix below); **(2) pitch-shifter full-spectrum rewrite + spectral BlockSize/Swaps/SwapSize/Omit — DONE (§12)** — P1 found to be single-partial, rewritten full-spectrum per user decision, awaiting hardware test; (3) noise mask (LPC-noisiness rank crossfader) — still to do. Now **46 params / 9 pages**, Engine 499,128 B / total 507,324 B (~17 KB margin); the noise masker (~8 KB) should still fit without a kit trim.

Resolved this session:

- ~~P4 was completely undefined~~ — resolved 2026-10-18 as a bitcrusher/sample-rate-reducer (both curve-only Contour destinations), later reworked to base+depth (2026-10-19), then **superseded entirely 2026-10-21: P4 is now a 12-stage allpass phaser** (see the phaser entry above; §5). The bitcrusher spec in §5-history/git is the record of the 2026-10-18→20 design.
- ~~Contour per-instance memory size conflict~~ — resolved: the ~47KB and ~13KB figures describe different `kMaxWin`/`kPieces` trim levels of the same formula (`size(W,P) ≈ 1328W + 160P + 928` bytes), not a real contradiction (§8).
- ~~The "~40 destinations" figure~~ — resolved. Peaked at 30; dropped to 21 (2026-10-20) when the 8 filter cutoffs left the Contour set (static bandpasses) and P3's fold+bias (2) became a single delay-time destination (1); rose to 22 (QCONT's shared filter-Q mod, +1); back to **21 (2026-10-21)** when P4's bitcrusher (bits+divisor, 2) became a phaser (center-freq sweep, 1): 16 feedback + P1 depth + P2 depth + P3 delay + P4 phaser sweep + filter-Q.
- ~~Filters, P3 processor, starter length, per-processor depth (2026-10-19/20 user requests)~~ — resolved: (a) **starter min lowered to 1 ms**; (b) **P3 wavefolder replaced by a plain 1–100 ms delay** (`delay.h`), base+depth like P1/P2; (c) **P4 reworked to base+depth** (randomized base bits/divisor, P4 depth scales the Contour modulation) and **P1/P2/P3/P4 all given depth controls** on the Depth page; (d) the per-channel **HPF+LPF replaced by a resonant bandpass** (Freq Hz + Bandwidth semitones, static Filters page) after the one-poles proved too gentle — the new filter measurably concentrates ~3× more kit energy in-band and drops the spectral centroid ~2 kHz (`test_filter_effectiveness.cpp`), and stays bounded even at 1-semitone extreme resonance (Reg contains it). All desktop + ARM validated.
- ~~P4 bitcrusher → 12-stage allpass phaser (2026-10-21 user request)~~ — resolved. *"Omit the bitcrusher and add a 12 stage phaser … just use its phase shifter with 12 stages as a processor instead of the crusher."* `p4_phaser.h`: 12 cascaded first-order allpass sections + dry/wet mix (`out=0.5(dry+wet)`), **no internal feedback, no LFO of its own** (the Contour system supplies the sweep). Coefficient `a=(t−1)/(t+1)`, `t=tan(π·fc/fs)` — puts each section's −90° point at fc; `a∈(−1,1)` so unconditionally stable at any fc/fs. 12 stages ⇒ a 6-notch comb (`|cos(6·φ₁)|`). Control model is base+depth like P1/P2: randomized per-render base center freq (300–1800 Hz) + a Contour-driven ±2000 Hz sweep scaled by "P4 depth" (one Contour destination replacing the bitcrusher's two, 22→21, §6/§8). Labels "P4 crush"→"P4 phaser". **A coefficient-sign bug (inverted `(1−t)/(1+t)`, which mirrored fc around fs/4 and swept the comb backwards) shipped briefly and was caught by the sweep-direction test**, then fixed — the measured-vs-analytic test alone did NOT catch it (both used the same wrong coeff), which is why a physical/semantic check was included. Verified in `test_p4_phaser.cpp` (6 notches, measured≈analytic, deep notch + unity peak, upward sweep, static bound, smooth-sweep stability 48k–384k, seed-reproducible base) + host harness; ARM clean; `p4_bitcrusher.h`/its test removed from the build but the header kept on disk for recovery. **On-device confirmation pending** (Step 16).
- ~~VCFRND / QCONT filter-modulation toggles (2026-10-21 user request)~~ — resolved. Two Off/On params on the Filters page (appended at the end of `parameters[]` as indices 35/36 per the ordering rule, §9). **VCFRND**: when On, `Renderer::startNewRender` draws fresh seed-derived (reproducible, log-uniform 20–16000 Hz) bandpass center freqs per channel, overriding the static Freq params — no Contour instance, randomized once per render. **QCONT**: when On, one shared Contour destination (the 22nd, §6) drives all 4 bandpass bandwidths ("Q") per internal sample, recomputing the biquad coeffs each sample in `advanceChunk`, overriding the static BW params. Both default Off (a pre-feature preset behaves identically). Terminology note: the filter is parameterized by **bandwidth in semitones**, which *is* its Q control (narrower = higher Q), so "modulate Q" = Contour-modulate that bandwidth. "a contour" (singular) → one shared curve for all 4 channels (also the memory-safe choice: +1,520 B vs +6 KB for per-channel, keeping ~13.2 KB pool margin). Verified reproducible + materially sound-changing (VCFRND 147%, QCONT 129% RMS change vs static, same seed/render) + bounded/NaN-free even with the per-sample Q sweep inside the resonant loop (`test_filter_mod.cpp`); end-to-end through the real factory (`test_plugin_host.cpp`); ARM clean. **On-device confirmation still pending** (Step 16).
- ~~Pops in playback + "render bleed" + single-contour redesign (2026-10-21 user report/request)~~ — resolved, three linked changes:
  - **Pops (clipping + loop seam).** Two causes. (1) Each channel is tanh-softclipped to ±11 V, but the mix `0.5·(4 channels)` can reach ±22 V, which the 12-bit kit writer **hard-clamped** at ±11 V → brick-wall clip pops. Fix: soft-clip the mix (`filters::softClip`, 11 V) **before** the AA filter, so it only asymptotes toward 11 V and never pins the hard-clamp rail (`test_renderer.cpp`: 0 rail hits, max ≈8.3 V). (2) **Loop seam** — the player loops `[0,N)` and the render ends on a ringing tail (≠0) while it starts cold (~0), so the seam `kit[N-1]→kit[0]` was a step → a click every loop. Fix: fade the first/last `kSeamFadeSamples` (256, ≈8 ms) of the baked kit to zero (`applyLoopSeamFades`, once at completion before swap), so the seam is 0→0 (`test_renderer.cpp`: kit[0]=kit[N-1]=0, mid non-silent).
  - **Render bleed (periodic cracking while a render runs).** The bake runs inside the real-time `step()`; its per-call cost (21 Contour engines + P1's FFT, ×`kChunkSize`) periodically exceeded the module's 90% CPU-mute threshold → bursts of muting on the live output. Primary fix = the single-contour redesign below (removes the dominant cost); `kChunkSize` left at 16 as the documented on-device fallback knob (§7).
  - **Single-contour "playhead" redesign (user: "to make the plugin simpler").** Replaced the 21 per-destination Contour engines with **one** curve baked per render into a 1,024-point cyclic table, read by **21 playheads** — each a seed-derived **phase offset in degrees**; a single base phase sweeps 0→360° over the render so every head does a full pass, offset-staggered so they don't move in lockstep (§6). Cuts per-sample CPU (1 LERP/destination vs 21 envelope generations) and DRAM (`sizeof(ContourDestinations)` 31,672 B → 5,792 B; Engine → 475,552 B, total 483,748 B, **~40.5 KB** pool margin). Deterministic from the seed (the curve was initially one fixed lifetime seed — see the per-render-seed entry below, which changed that). Verified in `test_contour_destinations.cpp` (all 16 crosspoint heads traverse the same shared curve over a full pass; decorrelated; deterministic) + full regression (18 suites) + ARM. **On-device confirmation pending** (Step 16) — especially that the render bleed is gone and the loop is click-free.
- ~~Contour should be randomised per render (2026-10-21 user request: "the contour should also be randomised based on its parameters and a new seed each render")~~ — resolved. Previously the single contour used one fixed lifetime seed, so the modulation gesture repeated every render (only the processor bases varied). Now `ContourDestinations::onNewRender(renderSeed)` takes the **per-render seed** (`baseSeed + renderIndex`, the same one the processor bases use) and re-derives **both** the baked curve shape **and** the 21 playhead phase offsets from it — a genuinely new gesture each render, still shaped by the Windows/Chaos/Radius/Round/Hold settings bank. Deterministic/reproducible for a given render index. **Costs nothing extra**: the single curve's table was already re-baked every render (`renderCurve` builds a fresh temp engine each time); only the seed now varies. Verified in `test_contour_destinations.cpp` (a new render seed → different gesture; same render seed → identical) + full regression (18 suites) + ARM. **On-device confirmation pending** (Step 16).
- ~~Quieter clicks still heard (user attributed to clipping) + a settable render level (2026-10-21 user request)~~ — resolved by a **Norm level** parameter (Render page, 0–11 V, kNT_unitVolts, def 8): the baked kit is **peak-normalized to this level at playback**. `Renderer` tracks the baked kit's true post-fade peak (`currentPeak_` = max of the non-faded body peak and the faded-edge max — tracking pre-fade over the whole buffer would overestimate when the peak sits in the starter region and make normalization undershoot, a bug caught by `test_renderer.cpp`), and the player multiplies every sample by `playbackGain() = level / currentPeak`. Because the player interpolates **linearly** (never overshoots its sample endpoints), the output peak is ≤ `level`, so any level ≤ 11 V is **clipping-free** — the headroom knob the user needed. It's applied at playback (one multiply, no extra pass, no completion spike) so it's also a **live** output-level control that rescales the current kit without a re-bake, and `level 0` = silence. Verified: normalizes to exactly the set level, never exceeds it, rescales live, silence at 0 (`test_renderer.cpp`); end-to-end peak ≈ level on the output bus (`test_plugin_host.cpp`). **On-device confirmation pending** — whether setting a level with headroom (e.g. 8 V) clears the quieter clicks.
- ~~SRAM pool size~~ — per 2026-10-18 confirmation, DTC 24KB / ITC 64KB / DRAM 512KB is the complete budget to design within; no separate SRAM allocation is being counted on.
- ~~Kit buffer bit-depth / sample-rate / duration~~ — **locked**: 12-bit packed/32kHz mono. Duration went ≈4.72s (estimate) → ≈4.60s (Step 14 ARM measurement) → **≈4.41s (141,000 samples, 2026-10-20)** after the DRAM-overcommit fix below.
- ~~Plugin went silent on hardware after the delay/bandpass/pitch changes~~ — **resolved 2026-10-20**. Desktop produced strong output (8.5V at defaults), so the DSP logic was fine — the fault was hardware-specific: the memory budgeting had only ever checked `sizeof(Engine)` and ignored the plugin's ~8 KB static `.bss` (mostly fft.h's `g_twiddle`), which the loader also places in RAM. The P3 delay buffer pushed the real total (bss 8,196 B + Engine 519,936 B = 528,132 B) ~3.8 KB OVER the 512 KB pool; the last build that fit (step 15, 522,292 B) had worked. Fixed by trimming the kit to 141,000 samples (Engine 501,328 B → total 509,524 B, ~14.4 KB margin) and tightening the `static_assert` to reserve 16 KB for the static footprint so it can't recur. Also added a denormal flush on the feedback loop state (cheap insurance against the Cortex-M7 FPU handling denormals more slowly than desktop x86 and spiking CPU). **Confirmation still needed from the module** — the module's *Misc / Plug-ins / View info…* will show whether the earlier build failed to load (memory) vs. loaded-but-silent (would point elsewhere).
- ~~P2 allpass coefficients and parameter spec~~ — resolved: coefficients derived via numerical optimization and verified to <0.12° error across 20Hz-20kHz (§3, Step 6); parameter spec settled as base+depth, ±2000Hz, same treatment as P1; full Processor implemented and validated on desktop and ARM (§3, Step 7).
- ~~P3 exact geometric fold formula and parameter spec~~ — resolved: formula derived from the mod+abs construction already locked in §4, `foldShape(u) = 1 - |wrap4(u+1) - 2|` (period 4, identity on [-1,1], reflects at every odd-integer boundary), verified numerically for identity, corner placement, global boundedness, and periodicity (§4, Step 8, `verify_fold.cpp`). Parameter spec settled as curve-only (fold, bias — both, no base), same treatment as P4; full stateless Processor implemented and validated on desktop and ARM (§4, Step 9).
- ~~What actually feeds S1–S4~~ — resolved: a math-generated square wave per channel (phase accumulator), 4 independent frequencies + 4 independent durations (1–500 ms since 2026-10-19), all static real parameters, fired once at render-start. No live audio capture, no Contour involvement, no memory impact — the plugin's original "no real-time connection between input and output" premise holds intact (§1).
- ~~Reg's AGC target level~~ — resolved: −18dB to −6dB relative to the hardware's ±11V output ceiling (≈1.39V–5.51V peak), locked 2026-10-18 (§1).
- ~~HPF/LPF/Reg implementation~~ — **done, 2026-10-18 (Implementation Plan Step 11)**: one-pole HPF/LPF pair and the full Reg chain (DC block → AGC → soft clip) implemented in `filters.h`, wired into `feedback_network.h` completing Σn → HPF → LPF → Pn → Reg → Out n. AGC targets -12dB (≈2.763V), the midpoint of the locked window. Validated on desktop and ARM — see §1's implementation-status note. One real bug (an unnormalized one-pole highpass with a sub-unity passband ceiling) was caught by numerical verification and fixed.
- ~~Contour instance management, destination wiring, and the output-unit mapping layer~~ — **done, 2026-10-18 (Implementation Plan Step 12)**: `contour_destinations.h` implements one `contourlazy::Engine` per real destination (30 total, final count) sharing one settings bank, plus the full output-unit mapping layer (direct linear for gains/integers, exponential for Hz-type cutoffs, reusing P3's own helpers for fold/bias) that §6 had left undesigned. Validated on desktop and ARM — see §6's implementation-status note. `sizeof` for all 30 engines measures exactly 45,240 bytes on both x86 and ARM — this measured figure later fed Step 14's whole-system sizing correction (§8).
- ~~Kit buffer and double buffering~~ — **done, 2026-10-18 (Implementation Plan Step 13)**: `kit_buffer.h` implements 12-bit packed/32kHz mono storage and a double-buffered store with an atomic (single-int-flip) swap. Validated on desktop and ARM — see §8's implementation-status note. Round-trip quantization error measured within one 12-bit step; write-order independence and swap atomicity both verified directly. (Sample count corrected from 151,188 to 147,204 in Step 14 — see below.)
- ~~Renderer state machine, render-chunk size, render-to-kit-rate conversion, 4-to-mono mixdown, and state continuity between renders~~ — **done, 2026-10-19 (Implementation Plan Step 14)**: `renderer.h` implements the chunked resumable render loop (`kChunkSize=16`/call, initial value, tunable), a fractional-accumulator decimator for arbitrary render/kit rate ratios, and a 0.5×-attenuated 4-channel mono mixdown (all this step's own documented decisions). **State continuity resolved**: processor/filter/Reg state resets cold at each render (only Contour carries level forward), to preserve seed-reproducibility — see §7's implementation-status note. **Critically, Step 14's whole-system ARM measurement caught that the per-category memory estimates had the kit ~9.67KB over budget**; the kit was resized to 147,204 samples (4.600125s) using real measured component sizes + a 2KB margin, and the reassembled Renderer now ARM-measures 522,240 bytes, 2KB under the 512KB DRAM pool. Validated on desktop (`test_renderer.cpp`) and ARM.
- ~~Output-unit mapping layer~~ — resolved in Step 12 (see that entry above); the gap §6 flagged ("Contour outputs linear volts; Hz-type destinations need a mapping layer") is now implemented and bounds-checked.
- ~~Full plugin integration: player, parameter set, factory, memory placement~~ — **done, 2026-10-19 (Implementation Plan Step 15)**: `4x4.cpp` is the loadable/playable build (scaffold archived at `History/4x4_v1.cpp`). 23 real parameters across 6 pages at the time of this step (later 24 with the Oversample control, §7); Output bus **does** get a paired Output Mode (`_WITH_MODE`); Regenerate is a momentary trigger (edge-detected in step() after the first hardware test showed the parameterChanged version didn't fire); the Contour settings bank and a Processors on/off page are both real adjustable pages (added after hardware testing); P1/P2 depth are 0-100% scales on the Contour curve; Play=Off = silence. Big Engine in DRAM (~514KB, ~10KB margin), small instance in SRAM. Validated end-to-end via a desktop host harness (`test_plugin_host.cpp`) driving the real factory entry points, plus a compile-time DRAM budget `static_assert`. See §6/§9 implementation-status notes.
- ~~Per-processor on/off~~ — resolved 2026-10-19 (user request after hardware testing): a "Processors" page with P1/P2/P3/P4 enum Off/On. Off mutes the channel's input (Σn=0) and output (0 to both mix and feedback), and skips the processor entirely (CPU saving). Render-side (affects the next bake, not an already-baked kit). Verified in `test_feedback_network.cpp` (disabled channel outputs exactly 0; all-off = silence) and the host harness.
- ~~Oversampling for less aliasing~~ — resolved 2026-10-19 (user request): a global Oversample factor (1x/2x/4x/8x) runs the whole network at the higher internal rate, then an anti-aliasing low-pass before the 32kHz decimation (§7). Ideal fit because the bake is offline — cost is bake time, not peak CPU, and zero extra memory. Measured ~18× less wavefolder alias energy at 4× (`test_oversampling.cpp`). Per-processor tradeoffs documented (P1/P2 degrade at high factors; 2×–4× recommended). The foundational AA downsample runs even at 1×, fixing the previously-unfiltered 48→32 fold.
- ~~Does Output bus need a paired Output Mode parameter?~~ — resolved YES (Step 15): follows the API's `_WITH_MODE` convention, as every audio output in the reference examples does (§6).
- ~~Play=0 behavior~~ — resolved: outputs silence (Step 15 confirmed §6's assumption; Replace mode writes zeros, Add mode contributes nothing).
- ~~Contour settings bank: real parameters or compiled constants?~~ — resolved: a real adjustable "Contour" parameter page (Windows, Chaos, Radius, Round prob, Hold prob), added 2026-10-19 on user request after the first hardware test. Global bank, per-instance seeds, applied at the next render start (§6/§9).
- ~~Regenerate trigger not firing on hardware~~ — resolved 2026-10-19: the `parameterChanged`-based momentary trigger didn't fire reliably on the module; reworked to edge-detect the live `v[]` value in `step()` (parameterChanged is now NULL). Verified in the host harness (fires, auto-resets, re-arms).
- ~~Contour memory vs. kit duration~~ — resolved: committed to a lazy/on-demand Contour reimplementation instead of the baked breakpoint-storage engine (§6/§8), cutting per-instance size from ≈7.34KB (trimmed) to a **measured** 1,504 bytes (native, untrimmed settings) — a **≈31× reduction**. This also fully resolves the former caveat about `kMaxWin`=4 capping Contour's "Windows" setting below its documented default of 6: the lazy design doesn't need any window-count trim at all, so Windows can use its full native range.
- ~~Lazy Contour engine implementation and measurement~~ — **done, 2026-10-18 (Implementation Plan Step 3)**: `contour_lazy.h` implemented, `sizeof()` measured (1,504 bytes), and validated against `contour_engine.h` with golden tests, dense sample-by-sample diffs (288,000+ samples per case), and a 2,000-run randomized sweep — zero discrepancies. Two real bugs were found and fixed during validation: (1) an envelope reaching its stage-2 target before its allotted time slot elapsed needs an implicit flat hold until the slot's boundary, mirrored from the reference engine's gap between consecutive `pushLv` calls; (2) a zero-distance stage transition (stage 1's target happening to equal stage 0's already-reached level) must be silently deduped, not emitted as a duplicate breakpoint, matching the reference engine's `pushLv` dedup behavior. ARM-compiles clean.
- ~~P1 base value treatment~~ — resolved: randomized fresh at each render, not performer-set; no longer a real parameter (§2). Depth remains a real parameter. This is the general rule for any future base+depth-style parameter too.
- ~~Playback model for now~~ — resolved (interim): a single **Play** parameter loops the whole kit buffer on/off, replacing the eventual sliced/polyphonic design for early testing purposes only (§6). The eventual design is unchanged, just not built yet — see "Sampler slicing method" below.
- ~~Output routing~~ — resolved: a real **Output bus** parameter selects where the player writes rendered kit audio (§6) — this closes the gap flagged in the previous session (no output-routing parameter had existed anywhere in the design).

New from this session, not blocking the rest of the plugin:

- **Is randomized-base generation seed-derived or true entropy?** P1's and P2's base (§2/§3) is randomized at each render — but whether that draw is seed-keyed (reproducible, consistent with every other random element in this design; this is what's implemented, with a fixed base seed + incrementing render index) or sourced from hardware entropy (e.g. `NT_getCpuCycleCount()`-style, breaking reproducibility for just this value) isn't finalized. The current implementation is reproducible by default; a hardware-entropy option can be swapped in behind the existing pluggable `randomBaseFn` hook without touching anything else.
- **AGC peak vs. RMS level detection (Step 11) — explicitly flagged, not a silent default.** Peak detection is what's implemented (§1 names it "the simpler default" and the plan required flagging this rather than quietly picking one). Not yet confirmed by ear on real self-oscillating content — RMS would react less to single-sample transients and more to sustained loudness, which could matter for how "pumpy" the AGC feels on this specific signal path. Needs a listening pass after Step 16's on-device testing.

Not blocking the rest of the plugin, but unresolved:

- **Render chunk granularity and resulting bake time:** an initial chunk size (`kChunkSize = 16` render samples/call) is now chosen and in place (Step 14), but the *final* value — the one that maximizes bake speed while staying under the CPU-overload threshold alongside the player and any other preset algorithms — is still only determinable by on-device profiling (Step 16), including whether Contour `rebuild()` calls need staggering across chunks.
- **Sampler slicing method:** overall model is settled — one continuous render, sliced afterward, triggered randomly and likely polyphonically (this is what makes double buffering effectively mandatory, §6) — but fixed-size vs. transient-detected slicing, exact slice count, and polyphony limits are still unspecified. The current interim Play-loop player (§6) is a placeholder standing in for this until it's built.
- **AGC vs. pitch-shifter latency interaction:** the AGC's fast attack (~2–10ms) operates in a similar time range to the pitch-shift leg's inherent block latency; possible periodic pumping tied to hop boundaries needs listening/tuning, not just calculation.
- **Kit-swap continuity:** double buffering guarantees no torn/partial reads, but not a smooth musical transition — if a loop is mid-playback when a freshly-baked kit swaps in, the waveform content can jump abruptly at that instant even though nothing glitches technically. Separate question from buffer safety, not yet addressed. (Interim-player-only; the eventual sliced/polyphonic player will re-arm this question differently.)

## 11. Platform constraints honored

- No dynamic allocation — placement `new` on firmware-provided static memory only.
- No exceptions, no RTTI, no STL, no threads, no OS/file calls inside `step()`.
- FFT implemented from scratch in portable float C++ (`<cmath>` only) since the API provides none.
- `step()` never blocks — render work is chunked as an explicit state machine, never a single long pass.
- Contour's reference engine already meets these constraints as shipped (no heap, double-precision via the Cortex-M7 hardware FPU, placement `new` only) — reused as-is, not re-derived.

---

## 11. Time-stretch (period-synchronous playback stretch) — 2026-10-22

Implemented per `period-stretch.md` (user request). **Playback-only**: the kit
audio is never modified; stretching is purely how the read head moves. Files:
`timestretch.h` (self-contained DSP), wired into `renderer.h` (analysis + map
double-buffering) and the `4x4.cpp` player.

**Confirmed decisions (2026-10-22):** Pitch and Stretch coexist (Pitch = the
per-sample advance/resample → pitch; Stretch = period-repeats → pitch-preserving
duration). Swap stays immediate on render-complete; the player keeps looping.
Analysis runs as a **post-render phase**, not inside the feedback render
(protects the render-bleed fix). Params exposed: Scale, Stretch, Threshold,
Xfade (new **Stretch** page).

**Scales (compile-time):** 10 octave-spaced targets 2,4,…,1024 ms, each searched
over `[L/√2, L·√2]`. Top capped at 1024 ms so a full-rate period fits a uint16 at
32 kHz and still repeats a few times in a 4.4 s kit.

**Period maps (per buffer, swap with the kit):** `PeriodMaps` = packed parallel
arrays `uint16 period[kTotalEntries]` + `uint8 conf[kTotalEntries]`,
`kTotalEntries = 2759` → 8,278 B/buffer, two buffers = 16.6 KB. Built once per
render by `timestretch::Analyzer`, chunked at `kAnalyzeHopsPerStep = 4` hops/step
during the renderer's new `kAnalyzing` state (render → finalize → analyze → swap).
Detection: zero-mean normalized autocorrelation over a decimated window (box-avg
for scales < 100 ms, rectified envelope for ≥ 100 ms), argmax lag → period,
peak → confidence. Reads are loop-wrapped so periodicity near the buffer end is
still found.

**Playback (`StretchReader`, one sample/call):** fractional read head advancing
by `adv = (32000/sampleRate)·pitchRatio`. **FINITE-repeat model (fixed 2026-10-22):**
on *entering* a slice it picks the scale (confidence-weighted score between the
two scales the knob straddles — never averages periods) and accrues the owed
repeats **once**: `debt += (sEff−1)`, `sEff = 1 + (stretch−1)·w` where `w` is the
soft Threshold ramp. At each period boundary it pays the debt down — repeat the
current slice while `debt ≥ 1` (jump back by `period`, linear crossfade
`min(period/2, Xfade)`, `debt −= 1`), otherwise advance to the next slice. Since
the owed amount is bounded (`sEff−1 ≤ 7`), every slice advances after a finite
number of repeats and **can never get stuck** (a safety cap `kMaxRepeats = 16`
backs this up). *Original bug:* debt was incremented every boundary, so at
Stretch ≥ 2× with high confidence it repeated forever — stuck on one slice; the
freeze-at-max special case was also dropped (user wants finite everywhere).
Loop wrap resets the read head; the read head also resets on every buffer swap
(`renderer.swapGeneration()` watched by the player). **At Stretch = 1.00× no
repeat ever fires → bit-identical to the old looping player.**

**Memory:** Engine 475,568 → **493,440 B**; with 8,196 B static bss total
**501,636 B, ~22 KB under the 512 KB pool** (static_assert slack ~14 KB). NOTE:
the two still-pending features (pitch-shifter spectral swap/omit ~11 KB; noise
mask analysis ~8 KB) will make the trio tight — the kit may need a small trim
when they land.

**v1 simplifications (flagged for on-device tuning):** analysis decimates by
box-avg/subsample (not a full anti-alias LP); no full-rate period refine and no
splice-time WSOLA; the crossfade length is counted in output samples. The linear
crossfade covers the ≤ D-sample period error at short scales. Add refine/WSOLA
later if short-scale stretch sounds clicky.

**Verification:** ARM-compiles clean; within DRAM budget. Per the 2026-10-22
workflow change, there is **no desktop test harness for this feature** — correctness
(does it find musical periods, do splices sound clean) is to be judged **on the
hardware**. Stretch = 1× passthrough identity is guaranteed by construction so the
base plugin is safe at default.

---

## 12. P1 full-spectrum rewrite + spectral manipulation (BlockSize/Swaps/SwapSize/Omit) — 2026-10-22

Implemented per `pitch-shifter-swap-omit.md` (user request, feature 2 of 3).

**Key finding first:** the spec assumed P1 was the full-spectrum "Ocean" shifter,
but the actual P1 was a **monophonic single-partial tracker** (kept only the
loudest bin, zeroed the rest) — leaving no spectrum for block swap/mute to act
on. Confirmed with the user; **chosen path: rewrite P1 full-spectrum**, 1024-pt
(no zero-pad, 511 operable bins), **percentages** for Swaps/SwapSize/Omit,
**swap before shift**.

**New P1 shifter (`p1_pitchshift.h`, full-spectrum Ocean bin-shift, no atan2):**
each source bin k's content moves to `dst = round(k_eff·ratio)` (k_eff = post-swap
bin), relocating the envelope by `ratio` ⇒ pitch shift; collisions sum, gaps stay
0. Phase coherence across hops is kept by rotating each bin by `exp(j·2π·Δ·hop/N)`,
Δ = dst−k the net displacement. For N=1024/hop=256 (hop/N=1/4) that's `j^(Δ·m)` at
frame m — multiply by {1, j, −1, −j} indexed by `(Δ·m)&3`, **exact for integer
displacements, no trig, no per-bin phase memory**. This dropped the old
prevPhase/synPhase arrays (~4 KB) and, crucially, avoids 511 atan2/hop — so it does
**not** re-introduce render bleed. At ratio=1 & no swap/omit it reconstructs the
input exactly.

**Spectral maps (per render, from the per-render seed ⇒ reproducible):**
BlockSize B∈{1,2,4,8,16,32,64} groups operable bins (1..511) into `numBlocks=511/B`
blocks (DC/Nyquist never touched; leftover bins pass through). Percentages map to
counts of the current B's maxima (Swaps/SwapSize ≤ numBlocks/2, Omit ≤ numBlocks).
**Omit** mutes random blocks with a raised-cosine edge ramp `R=min(B/4,4)`
(two-pass build so ramp writes can't be re-read as boundaries). **Swaps** pairs
random blocks at circular distance ≤ SwapSize (wrap), best-effort, as an
involution (energy-conserving). Per frame: **Omit (source) → Swap (blocks) →
Shift**; the phase rotation uses the net displacement regardless of order.

**Open-question defaults used** (user confirmed the 4 big ones; these are my
stated defaults for the rest): seed = per-render seed (reproducible); Swaps counts
= pairs; leftover bins pass through; no makeup gain after Omit (Reg AGC + playback
Norm compensate); muted blocks are still swappable (move silence); mute ramp
straddles the boundary.

**Memory:** Engine 493,440 → **499,128 B** (net +5.7 KB: −4 KB phase arrays, +work_
spectrum, +~5.6 KB maps). Total **507,324 B, ~17 KB under the pool**, static_assert
slack ~8.8 KB. The noise masker (feature 3) still fits without a kit trim.

**Verification:** ARM-compiles clean, within budget. Per the 2026-10-22 workflow,
**no desktop harness** — correctness (does the full-spectrum shift sound right;
do Swaps/Omit scramble/thin musically) is judged **on hardware**. Swaps=0/Omit=0 is
the clean full-spectrum baseline (the old single-partial sound is intentionally
gone — the user approved the rewrite).

**v1 flags:** full-spectrum bin-shift has the usual artifacts (summed collisions on
downshift, spectral holes on upshift, some phasiness on transients); the roots-of-
unity rotation is exact but the bin rounding is ±½ bin. Tunable if needed.
