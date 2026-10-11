# LIFT signal order

One fixed order, the same for every loop and every clip voice. Nothing in it
moves with patching: cables and pins only change *values* (CV) inside a stage.

## Per voice

```
loop T1..T4:  playhead read ─► GRAIN ─► PULSE ─► EQ / pan / fader ─┐
clip voice:   clip read     ─► GRAIN ─► PULSE ─► gain              ─┤
                                                                    ▼
                                         loop sum ─► cassette stage (optional) ─┐
live source (synth + drums + clip keys, or IN) ─► IN GAIN / THRESH ─► monitor ──┤
                                                                                ▼
                                                                  FX insert (bus)
                                                                                ▼
                                                     master safety limiter (-1 dBFS)
                                                                                ▼
                                    output ──► the DAW / speakers    and    capture ring (60 s)
```

1. **Source read.** The loop's playhead (shared transport position plus the
   loop's own offset): SPEED, SCRUB and REVERSE move this read position only
   (`tape_head`, `src/audio/process_block.cpp`). A clip voice reads its clip
   (rate = key pitch).
2. **GRAIN.** Reads its *own* fixed 8 s rolling buffer (see below), never the
   looper's buffer, so SPEED / SCRUB / REV and grain position never fight.
3. **PULSE.** Rhythmic gate / stutter / time-stop with its own small buffer
   (2 s).

   In this pass GRAIN and PULSE ship as **FX types** (FX page 2: the FX pad
   cycles on past COMP), so in the insert they run at the FX position, after
   the cassette, on the whole bus. The per-voice GRAIN / PULSE slots above are
   the defined order and pass through until per-loop instances exist (next
   step); GRAIN as SYNTH key 8 is a source voice and sits at step 1/2.
4. **Cassette stage.** Optional, off by default (SHIFT + white 7 cycles
   CASSETTE OFF → character 1..4 → OFF). On, the loop sum runs through the
   playback electronics and the record path through the record electronics
   (BIAS, REC LVL drive, hiss, wow). Off, the looper records the source straight
   (REC LVL = plain gain, unity at the default) and plays back clean.
5. **FX.** The insert sits on the bus after the loops and the cassette, with the
   live source monitor. Loops are recorded dry; the FX is heard (and resampled)
   live.
6. **Mix.** Master safety limiter (32-sample look-ahead, -1 dBFS ceiling,
   reported to the host as latency), then out.

## What gets kept

The capture ring is written **after** the master limiter: it holds exactly
what you heard, post chain. LIFT keeps the last N s of it; a REC pass keeps
the part of it heard while the pass ran. So the SELECT waveform always shows
the actually-captured audio (post GRAIN / PULSE / cassette / FX / limiter), not
a source buffer. Resampling a GRAIN effect therefore keeps the cloud as it
evolved over time, not a frozen window.

Every keep runs the trim stage first: integrated loudness (BS.1770, gated) to
-16 LUFS, then a 4x true-peak limiter at -1 dBTP. Keep → play → keep five
times stays within 1 dB (`lift_resample_check`).

## GRAIN buffers

* **GRAIN as a SYNTH engine (key 8)**: grains read the armed loop region
  (SPOOL-style source), position relative to that region.
* **GRAIN as an FX type**: a fixed **8 s** rolling buffer, written with the FX
  input every sample, independent of loop length and of varispeed (it is
  written at the bus rate, after the playheads). G POS = how far back in
  those 8 s, G SIZE = grain length.
* The G POS / G SIZE pin columns drive whichever GRAIN instance is active:
  the FX when the FX type is GRAIN and the insert is on, else the engine.

## Not in this order

Feedback cables (true graph cycles only, see `src/engine/patch.cpp`) get one
block (32 samples) of delay and a soft clip at the point where they close the
loop; the stage order above is unchanged by them.
