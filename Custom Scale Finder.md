# Chords — Plugin Documentation & Custom Scale Finder

A disting NT plugin (`Chords/Chords.cpp`). Runs a chord progression sequencer that drives three independent voices — a bass line, a melody line, and a 4-voice chord MIDI stream — all locked to the same scale, root, and progression.

---

## 1. What the plugin does

**Progression engine.** Eight progression steps, each holding a scale degree (I–VII). `ProgLength` (1–8) decides how many of those eight steps are actually played before looping back to step 0 — the other steps still hold real data, they're just not reached. Advance is driven by a CV/trigger input or the `Advance` parameter; `Reset` jumps back to step 0.

**Bass and Melody voices.** Each is a clocked, gated note generator with its own loop length, range, glide, and density. Each has a **Snap** switch (`BassSnap` / `MelSnap`):
- **Chord** (default, `=1`): the note pool is the current chord's 4 tones spread across several octaves — the voice is locked to the harmony of the current progression step.
- **Scale**: the note pool is 16 consecutive scale degrees from the root — the voice plays freely within Root + Scale and ignores the current chord entirely.

**Chord MIDI stream.** There is no audio chord oscillator. Instead, the current chord's 4 tones are sent out as MIDI notes on up to 4 channels (`Chord1CH`–`Chord4CH`), gated by its own clocked rhythm. Pitch is `Root + scale degree's chord tones`, shifted by `ChordOctave`, and re-voiced by `Inversion` (0–3): Inversion shifts chord voices `0..inv-1` up an octave, so the *inv*-th tone becomes the lowest note (root position → 1st → 2nd → 3rd inversion). Each MIDI channel keeps its own identity (Chord1CH is always "the root-position tone class", etc.) — inversion moves octaves, not which channel carries which tone.

**Generator.** `Generate!` randomises all 8 progression degrees in one shot, using the current scale's harmonic functions (Tonic/Subdominant/Dominant) to keep the result musically sane. It always writes all 8 steps regardless of `ProgLength`, so lengthening the progression later doesn't expose blank steps.

### Parameter pages

| Page | Contains |
|---|---|
| **Progression** | Deg1–Deg8, ProgLength, AdvanceIn, Reset, ProgDiv, Advance |
| **Bass** | Waveform, Level, Detune, Bass2 Level, Output, ClockIn, Range, Offset, Randomise, LoopLength, Glide, GlideCurve, Density, NoteUpdate, Octave, **BassSnap** |
| **Melody** | OSC1 Wave, Algo, Index, OSC2 Part/Semi/Detune, LFO Speed/Shape/Reset/Amount, Level, Output, ClockIn, Range, Offset, Octave, Randomise, LoopLength, Glide, GlideCurve, Density, NoteUpdate, **MelSnap** |
| **Generator** | Scale, Root, Inversion, Generate!, **RandScale** |
| **MIDI** | BassCH, MelCH, Chord1–4CH, ChordDensity, MelDensity (shared), MIDIOut |

There is **no Chord page and no QuadLFO page** — both were removed once the chord voice became MIDI-only. Their parameters (OSC1-4 Level/Shape, Detune, FMFreq, ChordIndex, ChordAlgo, ChordWave, Hermite, Phase, Output, ChordOctave, LevelFreq/Mod, ShapeFreq/Mod, AmpLFOShape, ShpLFOShape, ResetOnChrd) still exist internally at their original indices so old CV/MIDI mappings don't break, but they do nothing visible and aren't on any page. `ChordOctave` is the one exception that's still functional — it still shifts the Chord1-4 CH MIDI notes, it's just not exposed on a page right now.

`GenProgLen` — a second, separate "ProgLength" that used to sit on the Generator page — was deleted from view for the same reason: it never did anything. The **real** ProgLength is the one on the Progression page.

---

## 2. Scales: built-in, hand-picked, and Custom

`Scale` (Generator page) now has 28 options:

- **0–9**: the original 10 built-in scales (Major, Nat Minor, Dorian, Mixolydian, Lydian, Harm Min, Mel Min, Dbl Harm, Hung Min, Hirajoshi). These have curated `kScaleChordTypes` rows, so their 4-voice chords use proper functional voicings (e.g. Natural Minor's V borrows a leading tone for a real dominant instead of a plain minor triad).
- **10–26**: 17 hand-picked scales kept permanently from earlier Custom-slot research (Bence, Ilona, Csaba, Emma, Levente, Zita, Elek, Nora, Botond, Vera, Zsolt, Blanka, Huba, Kata, Imre, Luca, Barna). These have no curated chord table — their chords are derived mechanically (stacking every other scale degree from the root), same mechanism as Custom below.
- **27 — Custom**: the live research slot, unchanged in behaviour, just moved to the end of the list.

## 3. The Custom Scale Finder

An experimental, throwaway research mode for auditioning unconventional 7-note scales beyond the 27 fixed ones above. Nothing here is saved to disk — it's a way to try scales live and copy down the ones worth keeping.

### How to use it

1. Set **Scale** (Generator page) to **Custom** (the last option, after Barna).
2. Trigger **RandScale** (Generator page). This rolls a brand-new random 7-note scale and immediately applies it — the progression, bass, melody, and chord MIDI all start using it right away.
3. Read the two-line display that appears on screen only while Scale = Custom (nothing shows for any other scale index).
4. Keep rolling until you hear something worth keeping, then write down the **top row** by hand.

Rolling does **not** touch the Scale parameter itself — you stay on Custom the whole time, so you can keep re-rolling without interrupting anything else.

### Reading the two rows

**Top row — the scale**, e.g. `0 2 3 5 7 8 10`
The 7 semitone offsets from the root, ascending, first one always `0`. This is formatted to match exactly the syntax of a row in the plugin's built-in scale table (`kScaleIntervals[]` in the source) — it's literally the line you'd type in by hand to make a good roll permanent in a future build.

**Bottom row — the shape**, e.g. `2 1 2 2 1 2 2`
The gap in semitones between each consecutive note, with the last number being the gap back up to the octave (the row always sums to 12). This doesn't care what the root is — it's a fingerprint of the scale's step pattern, independent of which note is "home."

### Why the bottom row matters

The same 7-note collection can produce a completely different-looking top row depending on which of its notes gets treated as the root — that's what a musical *mode* is. Major and Natural Minor are the same 7 pitch classes, just rooted on a different degree; their step rows are cyclic rotations of each other:

```
Major:         2 2 1 2 2 2 1
Natural Minor: 2 1 2 2 1 2 2
```

The random scale generator always forces `0` (the root) into the set, so a re-roll can hand you a top row you don't recognise, but whose bottom row is a rotation of one you already wrote down. That means it's the *same* scale color, just entered starting from a different note — not a new discovery.

**Practical workflow:** when you find a roll worth keeping, write down both rows. Before writing down a *new* roll, check whether its bottom row is a rotation of one already on your list. If it is, you already have this scale — skip it and keep rolling. If it isn't, it's genuinely new.

### Display notes

- The top row is drawn 7px lower than its first placement, at y=17 (just above the progression frame).
- The bottom row is at y=60 (just below the frame).
- Both drop to a smaller font automatically if the text would run past roughly 22 characters, since there's no way to measure exact pixel width from plugin code — this is a conservative estimate, not a precise fit.

### Turning a roll into a permanent scale

The Custom slot is scratch space — it resets to Natural Minor every time the module powers up or the plugin reloads. This is exactly how the 17 scales in section 2 got made permanent. To do the same with a new roll:

1. Write down its top row (the interval list).
2. In `Chords.cpp`, insert it as a new row in `kScaleIntervals[]` *before* row 27 (Custom) — rows 10–26 are the reserved block for named scales, so a new one goes at the end of that block, pushing Custom's index up by one. Give it a name in `scaleNames[]` in the matching position, and update `kScaleCustom` and the `.max` on the `Scale` parameter to match Custom's new index.
3. It will **not** get a curated chord-quality row in `kScaleChordTypes[]` automatically — that table only covers scales 0–9 and is hand-curated per scale (it's what lets Natural Minor's V chord borrow a leading tone instead of mechanically stacking a plain minor triad, for example). New scales — like all 17 in section 2 — use the mechanical "stack every other degree from the root" formula (`chordToneSemis()`) instead, which is already wired up for any scale index ≥ 10.
