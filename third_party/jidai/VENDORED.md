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
