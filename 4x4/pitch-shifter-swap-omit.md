# Pitch shifter update: BlockSize, Swaps, SwapSize, Omit

## Goal

Extend the existing frequency-domain pitch shifter (Ocean algorithm, Juillerat 2010) with four new parameters that rearrange and thin out the spectrum:

- **BlockSize** – how many adjacent FFT bins are grouped into one block; all swapping and muting works on whole blocks.
- **Swaps** – how many pairs of blocks exchange places.
- **SwapSize** – how far from its original position a block may be swapped (wraps around the spectrum edges).
- **Omit** – how many blocks are muted, applied *before* the pitch shift.

The swap map and mute map are randomised once at the start of every render and held fixed for the whole render.

**Before writing code, read the existing pitch shifter implementation and confirm the open questions at the end of this document with me. Do not silently pick defaults for anything listed there.**

---

## Current shifter (do not change its behaviour at zero settings)

- 1024-sample analysis block, Hann window
- 4× overlap, hop 256
- 2× zero-padded FFT: 2048 points, 1025 real-spectrum bins (0 = DC, 1024 = Nyquist)
- Bin shift applied with precomputed roots-of-unity complex multiplication; no atan2 / phase unwrapping
- Overlap-add resynthesis
- Hand-written forward/inverse FFT in portable float C++ (`<cmath>` only), statically sized buffers; the disting NT API has no FFT/DSP library
- The shifter sits inside the render feedback loop; its latency is part of the loop's character

**Regression requirement:** with `Swaps = 0` and `Omit = 0`, output must be bit-identical (or numerically identical within float noise) to the current implementation, at every BlockSize.

---

## Key concept: blocks

With 2× zero padding and a Hann window, one sinusoidal partial spreads over roughly 8 adjacent padded bins, and adjacent padded bins are strongly correlated. Swapping or muting single bins cuts partials apart and sounds noisy/phasey; larger blocks move partials (or groups of partials) intact.

All swap and mute operations work on **blocks** of `B` contiguous padded bins, where `B` is the **BlockSize** parameter. Every bin in a block moves by the same displacement.

- Operable range: padded bins **1 … 1023**. DC (0) and Nyquist (1024) are never swapped or muted.
- `numBlocks = 1023 / B` (integer division). Blocks cover bins `1 … numBlocks·B`. Leftover bins above that (`1023 mod B` of them) pass through untouched.

| BlockSize B | Width at 48 kHz | numBlocks | Leftover bins | Max Swaps / SwapSize | Max Omit | Character |
|---|---|---|---|---|---|---|
| 1 | ~23 Hz | 1023 | 0 | 511 | 1023 | Partials torn apart, gritty/noisy |
| 2 | ~47 Hz | 511 | 1 | 255 | 511 | Very grainy |
| 4 | ~94 Hz | 255 | 3 | 127 | 255 | Fine-grained, partials start to break |
| 8 | ~188 Hz | 127 | 7 | 63 | 127 | ~one partial per block, clean (default) |
| 16 | ~375 Hz | 63 | 15 | 31 | 63 | Small groups of partials |
| 32 | ~750 Hz | 31 | 31 | 15 | 31 | Coarse, regions swap |
| 64 | ~1.5 kHz | 15 | 63 | 7 | 15 | Very coarse, re-EQ-like; top ~1.5 kHz untouched |

Bin width = fs / 2048 (about 23.4 Hz at 48 kHz).

---

## Parameters

All four are integer/enum parameters, **latched at render start**: changes take effect at the next render, never mid-render (phase coherence requires a fixed map, see below).

| Name | Type / range | Default | Meaning |
|---|---|---|---|
| BlockSize | Enum: 1, 2, 4, 8, 16, 32, 64 (padded bins) | 8 | Bins per block for swap and mute |
| Swaps | 0 … `numBlocks / 2` | 0 | Number of disjoint block pairs that exchange places |
| SwapSize | 1 … `numBlocks / 2` | 1 | Max circular distance, in blocks, between the two blocks of a pair |
| Omit | 0 … `numBlocks` | 0 | Number of blocks muted before the shift |

### Ranges that depend on BlockSize
The maxima of Swaps, SwapSize and Omit depend on BlockSize (see table above). Handling, in order of preference:

1. **If the disting NT API allows updating a parameter's min/max at runtime**, update the ranges of Swaps, SwapSize and Omit whenever BlockSize changes, clamping their current values. Check the API headers before assuming either way.
2. **Otherwise**, declare the parameters with the ranges for B = 1 (Swaps 0–511, SwapSize 1–511, Omit 0–1023) and clamp to the current maximum at render start. The effective (clamped) values are what the maps use.

See open question 4 about a percentage alternative.

Notes:
- SwapSize max is `numBlocks / 2` because with wrap-around any larger circular distance is equivalent to a smaller one in the other direction.
- Derive all ranges from constants and `B` in code; don't hard-code the table numbers.

---

## Map generation (once per render)

Run at the start of each render, before the first frame. Cost is O(numBlocks + 1025); no heap allocation.

### RNG
- Small deterministic PRNG, e.g. xorshift32. No `<random>`.
- Seeded once per render (seed source: see open questions).
- Generate the **mute map first, then the swap map**, from the same RNG stream, so a given seed (plus parameter values) reproduces both.

### Mute map (Omit)
1. Pick `Omit` distinct blocks uniformly at random (partial Fisher–Yates over the block index array).
2. Build a per-bin gain array `muteGain[1025]`, initialised to 1.0, set to 0.0 for every bin in a muted block.
3. Soften edges: at every boundary between a muted and an unmuted block, apply a raised-cosine ramp `R` bins wide on each side of the boundary, with `R = min(B / 4, 4)` (integer division, so `R = 0`, i.e. hard edges, for B < 4; R = 2 at B = 8; R = 4 at B ≥ 16). Adjacent muted blocks form one continuous hole with no ramp between them.
   - Reason: a hard brick-wall hole rings in the time domain and wraps around the circular IFFT as pre-echo/glassy ringing. Scaling R with B keeps small blocks from being ramped away entirely.
4. Bins 0 and 1024 and the leftover bins above the last block keep gain 1.0 (except where a ramp from the last block's boundary reaches them).

### Swap map (Swaps, SwapSize)
Output: `blockDst[numBlocks]`, initialised to identity (`blockDst[i] = i`).

1. Keep a `paired[numBlocks]` flag array.
2. Visit blocks in a random order (Fisher–Yates shuffle of block indices). For each unpaired block `a`, while `pairsMade < Swaps`:
   - Try up to `K` attempts (proposed `K = 16`): pick `d` uniformly in `1 … SwapSize` and a random sign; `b = (a ± d) mod numBlocks` (**wrap around**).
   - If `b != a` and `b` is unpaired: set `blockDst[a] = b`, `blockDst[b] = a`, mark both paired, `pairsMade++`, move on.
   - If all attempts fail, skip `a`.
3. This is best effort. With high Swaps and small SwapSize it may make fewer pairs than requested. That is acceptable; don't loop forever.
4. Expand to a per-bin table `swapDst[1025]` (int16): identity everywhere, and for each bin in block `i`, `swapDst[bin] = bin + (blockDst[i] - i) * B`.

Invariants (assert in a debug/host build):
- `blockDst` is an involution (`blockDst[blockDst[i]] == i`), so the mapping is one-to-one; no collisions, no summing, energy conserved.
- Every pair satisfies circular distance ≤ SwapSize.
- `swapDst[0] == 0`, `swapDst[1024] == 1024`, leftover bins map to themselves.

---

## Per-frame processing order

For each analysis frame `m` (keep `m` modulo the rotation-table period):

1. Forward FFT as now.
2. **Omit:** multiply each source bin `k` by `muteGain[k]` (skip the bin entirely if the gain is 0).
3. **Shift:** existing Ocean mapping, source bin `k` → `shiftDst(k)`.
4. **Swap:** final destination `dst = swapDst[shiftDst(k)]` (only if `shiftDst(k)` is inside 0…1024; otherwise drop the bin as the shifter already does).
5. **Phase rotation:** use the **total** displacement `Δ = dst − k`, not just the shift displacement, when indexing the existing roots-of-unity table. With hop 256 and FFT size 2048 the extra phase advance per hop is `2π·Δ·256/2048 = 2π·Δ/8`, i.e. table index `(Δ · m) & 7`. Verify this against the existing code's constants rather than trusting this number.
   - This is what keeps swapped content phase-coherent across frames (clean inharmonic result instead of phasey smear). It only works because the map stays constant for the whole render.
6. Inverse FFT and overlap-add as now.

Swap is applied **after** the shift by default (fixed-frequency scrambling: the same spectral regions exchange regardless of pitch). See open questions.

---

## Constraints

- Portable float C++, `<cmath>` only; no new libraries.
- All new buffers statically sized for the worst case (B = 1, numBlocks = 1023): `swapDst[1025]` (int16), `muteGain[1025]` (float), `blockDst[1023]` (int16), `paired[1023]`, shuffle scratch `[1023]` (int16).
- No allocation, no blocking I/O, nothing unbounded in the audio path. Map generation is bounded (≤ K attempts per block).
- Keep the hand-written FFT untouched.
- Follow the plugin's existing parameter, naming and serialisation conventions.

---

## Testing / acceptance

- `Swaps = 0`, `Omit = 0` → identical output to the current build, at every BlockSize.
- Host-side (desktop) test for the map generator, run for every BlockSize: invariants above, pair counts, distances with wrap, leftover bins untouched, reproducibility for a fixed seed.
- Range handling: changing BlockSize clamps Swaps/SwapSize/Omit correctly (runtime range update or render-start clamp, whichever was implemented).
- `Omit = max` → output contains only DC/leftover bins (near silence at small B; top band survives at B = 64); no crash, no NaN.
- `Swaps = max`, `SwapSize = 1` → subtle detune/shimmer; `SwapSize = max` → strongly inharmonic. Compare B = 1 vs B = 8 vs B = 64 by ear.
- Check CPU on hardware: per-frame cost should rise only by a table lookup and a multiply per bin; map generation at B = 1 is the worst case.

---

## Open questions — confirm with me before implementing

1. **Swap order:** swap **after** the shift (fixed-frequency scramble, the default above) or **before** the shift (scramble follows pitch)? Or a mode switch?
2. **Swaps × Omit interaction:** if a muted block gets swapped, it simply moves silence (current spec). Should muted blocks instead be excluded from swapping so every swap moves audible content?
3. **Seed:** fresh random seed each render (from what entropy source?), or a stored/visible seed so a good render can be reproduced? Should the last seed be saved with presets?
4. **Count vs percentage:** if parameter ranges can't follow BlockSize at runtime, should Swaps and Omit be percentages (0–100 % of the maximum) instead of counts, so the full knob travel is always useful?
5. **Leftover bins at large BlockSize:** at B = 64 the top ~1.5 kHz (63 bins) is never swapped or muted. Acceptable, or should the leftover form a smaller final block that can be muted/swapped?
6. **Level after Omit:** muting lowers the level. Leave it (current spec), apply approximate makeup gain `√(1 / (1 − mutedFraction))`, or something else?
7. **Mute edge ramp:** is `R = min(B/4, 4)` straddling the boundary acceptable, or should the ramp sit fully inside the muted block?
8. **Swap counting:** the spec counts pairs (each swap moves two blocks). Did you mean the number of pairs, or the number of blocks that move?
9. **Which plugin/file:** confirm the target plugin and source file(s) of the pitch shifter.
