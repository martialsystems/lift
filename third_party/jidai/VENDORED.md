# Vendored Jidai code (Martial Systems, proprietary, same owner as LIFT)

Copied, not submoduled, so LIFT builds alone and the DSP core can move to a
hardware target unchanged. Nothing here depends on JUCE or any GUI.

| Folder | Source repo | Path | Commit |
| --- | --- | --- | --- |
| `shogun/` | github.com/martialsystems/shogun | `engine/` (drum engine, voices, DSP blocks) | 4b3ac2d54fb0a41335f01ba9788f533894e6a83a |
| `jidai-common/` | github.com/martialsystems/jidai-collection (as vendored by SHOGUN's `third_party/`) | `jidai-common/` 1.1.2 | 8a4b5ae (see `jidai-common/VENDORED-upstream.md`) |

Third-party check: SHOGUN's `third_party/` holds only jidai-common; neither
folder carries third-party or copyleft code (all files are Martial Systems
copyright). The halfband coefficients are SHOGUN's own design.

LIFT uses: SHOGUN's 14 drum voices and `shogun::Engine` (src/engine/drums.*),
its DSP blocks (PolyBLEP, ZDF ladder, OTA SVF, RC envelopes; src/engine/synth.*),
and jidai-common's `jcs::Graph` feedback classification and run order
(src/engine/patch.*), the same approach as Ronin's PatchGraph.

LIFT changes to the vendored copy (kept small, marked "LIFT" in the source):
- `shogun/voices/cp.h`: `CpVoice::setShape(gap, burst)` makes the clap's burst
  spacing and burst decay settable (SHOGUN fixes them at 11 ms / 3 ms); the
  909 kit uses 7.5 ms / 2.2 ms for its tight clap.
- `shogun/voices/bd2.h`: `Bd2Voice::setBendTau(s)` (SHOGUN: 80 ms); the 909 kick
  drops its pitch with 40 ms.
- `shogun/voices/toms.h`: `TomVoice::setShift(oct)` transposes the tom range
  (the 909 toms sit lower, -0.6 oct).
- jidai-common's `dsp/Halfband.h` is also used by the DRIVE effect's 2x oversampling.
