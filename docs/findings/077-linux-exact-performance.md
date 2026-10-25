# 077: Linux exact renderer submission and readback costs

2026-10-24, continuation of fork issue #1 and finding 076. This is a measured
performance improvement, not completion of the issue's correctness/performance acceptance.

## Feedback loop

`port/tools/bench_exact_linux.py` builds the existing GS replay for Linux and
replays player-owned captures through the real Vulkan callbacks. It measures
GIF processing, compute completion and CPU scanout, excluding Vulkan startup,
shader compilation and image-file output. `--reference` compares all 4 MB of
final VRAM; `--max-ms` makes the observed slowness fail explicitly. It reads the
reference before running, so overwriting an output cannot hide a difference.
Captures and generated VRAM files are untracked and must never be committed.

Build and reproduce (DXC must be on PATH or specified in the environment):

```
python port/tools/bench_exact_linux.py /path/to/capture.gs --build --max-ms 33.333
```

The minimized active-menu repro is the existing three-field capture at
`/tmp/dhd-issue1-vulkan-accept/frame-001.gs`. Single-field captures with only the
initial freeze did not reproduce the rendering cost and were rejected as the
performance seam. The original three-field replay failed at 53.689 ms/field;
33.333 ms is the raster budget for 30 fields/s, not a whole-game FPS guarantee.
The same loop passed after the changes. Heavy racing captures still exceed it.

## Diagnosis and changes

Ranked hypotheses were excessive small submissions, CPU snapshot/VRAM copies,
shader cost, and SDL presentation. Temporary targeted timing found 493 dispatch
batches and 55,550 primitives in the three menu fields: 136.947 ms inside flush,
120.504 ms waiting for fences, and 12.017 ms copying 33 texture snapshots.
The replay has no SDL window, so presentation cannot explain that cost.
All temporary `[DEBUG-perf1]` production instrumentation was removed.

Grouping submissions alone did not improve the original replay substantially.
The cause was a combination of unnecessary full synchronization for unrelated
CLUT/transfer ranges, per-batch host waits, CPU texture snapshots, and disabled
existing shader fast paths. The accepted implementation:

- Copies back only dirty pages intersecting `sync_range`; untouched ranges do
  not flush draws. Wrapped/out-of-bounds requests retain full synchronization.
- Records up to 16 ordered commands per submission. Each dispatch has separate,
  aligned constants, primitive data and tile-list storage. Slots are reused only
  after the submission fence completes. This adds about 61 MiB of input storage.
- Copies texture snapshots using ordered Vulkan buffer transfers. Barriers cover
  host writes, compute reads/writes and transfer reads/writes; host access drains
  queued commands before touching shared GPU storage.
- Enables the existing shared HLSL early-depth and exclusive-word-store paths
  with the same `gs_raster_independent_pixels` proof as D3D11. Packed formats
  retain CAS, and aliases retain the conservative path. Shader arithmetic and
  coverage rules are unchanged.

## Results

Host: i686 RADV RENOIR. Five interleaved baseline/after replays, identical
captures, all full-VRAM comparisons MATCH:

| Capture | Baseline median ms/field | After median ms/field | Raster speedup |
| --- | ---: | ---: | ---: |
| Active menu, frame-001 | 53.217 | 31.713 | 1.68x |
| Race, frame-009 | 80.937 | 42.999 | 1.88x |

All five menu-after samples were 31.340..32.123 ms/field, versus
51.817..54.663 before. All eleven available captures (frame-000..010) also
produced byte-identical final VRAM against the original backend. Timing and
hash evidence: `/tmp/dhd-perf-suite/results.json` and `repeated.json`.
These are comparisons to the original Vulkan implementation, not proof of
independent retail or D3D11 equivalence.

A windowed GDB sample of fields 420..480, with the same screen-keyed public-pad
navigation, measured 60 menu-frame increments in both versions: 9.892768 s
before (6.065 fields and menu frames/s), 6.832267 s after (8.782/s), about 45%
more throughput. Logs: `/tmp/dhd-live-base.log`, `/tmp/dhd-live-after.log`.
These are monitored samples on a shared host, not clean end-user FPS promises;
GPU test/build activity occurred during the broader validation session. The
old finding-076 sample is not substituted for this run's baseline.

## Validation and limits

- Shared exact GPU fixtures: 0 failures, including batching, aliases, packed
  writes, feedback, depth, local transfers and deferred readbacks.
- Added 48 changing-state point draws without intervening CPU reads to cross
  slot reuse boundaries. Independent literal pixel expectations pass. A negative
  control that deliberately reuses slot-zero constants/primitives fails this
  test and the broader suite (20,538 failed checks).
- GS memory/GIF/CLUT: 3,000 transfers, 6,657,394 pixels, 0 differences.
- Optimized SPIR-V passes `spirv-val --target-env vulkan1.2`.
- Updated runtime compiled and linked, executable refreshed at
  `build/port/link/dhd` and `build/port/run/dhd`; actual windowed boot/menu pass.
- Python compilation and `git diff --check` pass. `src/` and matching checks
  are unchanged. No rebuilt retail ELF is available here, so no fresh retail
  image MATCH is claimed (the checker requires a built ELF argument).

Issue #1 remains open: heavy race rasterization alone still exceeds 33.333
ms/field, and full-game menu throughput is still below real time. VU work,
scanout and other game costs are outside this replay measurement. D3D11/
independent-retail parity and the two race outliers from finding 076 remain
unresolved; input/close acceptance from that finding is not re-claimed here.

## Sources

Consulted Khronos' [synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html),
[synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
and [memory allocation specification](https://docs.vulkan.org/spec/latest/chapters/memory.html)
for host-coherent readback, transfer/compute barriers and mapped-memory costs.
The existing repository D3D11 backend and shared raster shader supply the
independence proof and optimized shader paths. No external code was copied.
