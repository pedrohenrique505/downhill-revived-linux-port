# 080: Linux exact picture and readback acceptance

2026-10-25, fork [issue #1](https://github.com/pedrohenrique505/downhill-revived/issues/1).
Continuation of findings 076–079. The remaining issue checkbox is comparison
against PCSX2, including explaining or fixing every pixel with error above 16.

## Reproducible reference tooling

`prepare_pcsx2_compare.py` now builds the pinned PCSX2 2.8.2 adapter with CMake
on Linux. `--gsrunner` selects its headless replay runner; the default builds
the Qt executable for independent retail boot. `--prefix` accepts a local
dependency installation, and `--jobs` bounds build parallelism. The runtime
resources are copied from the downloaded source; no BIOS or game is copied.
Linux uses bundled FFmpeg headers to avoid coupling comparison to the host's
newer FFmpeg API. OpenGL is needed even for software GS output.

The adapter can export synchronous FIFO read responses with
`DHD_COMPARE_READBACKS`, in the same length-prefixed format as
`port/gs/replay.c`. This observes returned bytes without changing GS math.

All new captures and reference outputs stay in ignored
`build/port/issue1-picture/`, rather than ephemeral `/tmp`.
The current executable's race fields 3000–3002 reproduce byte-identical RGB
through the isolated Vulkan replay (zero different pixels in all three fields).
Those three fields contain no FIFO reads, so they do not validate readbacks.

The nine existing comparison/input/crop tests pass. No matching source or
existing acceptance tolerance is changed. Final image acceptance is still open.

## Constant Gouraud channels

The original eight-field riding replay fails identically twice: field 3 has
four display pixels above 16 (two GS pixels doubled vertically), maximum 80;
field 6 has two, maximum 26. Isolated per-pixel draw probes on both renderers
locate missing alpha-tested triangles, rather than a display crop mismatch.
One triangle alone still fails with maximum 34. Probe-induced synchronization
does not change the original Vulkan picture.

Three ranked predictions were coverage differences, texture interpolation,
and Z/order. The candidate pixel lies strictly inside its triangle. A synthetic
constant-alpha textured Gouraud triangle reproduces the discard without game
data: `(0,0), (496,1), (2,512)` in GS 12.4 coordinates, constant RGBA 128,
texture alpha 65, ATST GREATER 64. Pixel (16,1) fails the new GPU assertion
before the fix. Summing three weighted copies of 128 yields 127.99999 and
truncates to 127, then MODULATE produces 64, incorrectly failing alpha.

Interpolating channel differences from the first vertex preserves constant
channels. The synthetic GPU regression now passes; all existing GPU tests and
the 3,000-transfer/6,657,394-pixel GS suite pass. Original riding field 3 now has
no pixel above 16 (maximum 13), removing four of six outliers. Field 6 still has
the same two pixels at maximum 26 and remains under investigation. No full
acceptance or new matching-image result is claimed by this partial fix.

## Remaining riding outlier: PCSX2 software edge rounding

After the alpha fix, field 6 differs above 16 only at display (474,242) and
(474,243): a single GS pixel (474,121) doubled by interlaced scanout. Reference
RGB is (126,97,103), exact is (100,81,77); maximum channel error is 26.
The raw comparison deliberately still fails; there is no excluded pixel,
threshold adjustment or added alignment search in the checker.

The first diverging primitive is native draw 7,638 in the preceding rendering
interval. Raw GS vertices are (38090,31735,192580), (40253,29658,244760),
(35685,33386,104561), with XYOFFSET (28672,30984). The sample is exactly on
the left edge from vertex 1 to vertex 2: its edge function is **zero**, with
the other two edge functions -178009 and -1246063 (area -1424072).

Exact includes that left edge and writes Z=122085. PCSX2 software excludes it,
leaving Z=0, then a later primitive writes Z=55426 and another colour. A
single-triangle reduction still differs at exactly those two display pixels
(maximum 36 before later blending). Instrumenting PCSX2's
`GSRasterizer::DrawTriangleSection` gives:

```
top=121 left=474.000030518 right=480.738677979 ceil_left=475
```

This is a reference-side floating-point scanline rounding discrepancy. Sony's
GS User's Manual v6.0, section 3.2.9, printed page 45 / figure 3-7, specifies
inclusion of a sample on a left edge. The port's exact integer edge test follows
that rule; copying the software renderer's float error would break it. The
documented exception is this traced primitive/pixel only, not a general waiver
for future outliers. Relevant evidence is in `triangle-edge/`,
`triangle-edge-check/`, `triangle-edge-probe/`, `probe-z.log` and
`pcsx2-z-probe/`, all under `build/port/issue1-picture/`.

## Capturing real reads and race-clock moments

The old native capture writer emitted GIF, register and VSync records but never
FIFO read records. Empty replay transcripts therefore could not establish
readback correctness. `Gs.record_read` now records successful immediate/deferred
read requests at issue time, flushing earlier GIF packets before the read marker.
The callback is absent in ordinary play. Deferred settlement does not emit a
second marker. Non-qword diagnostic requests are refused rather than rounded.

The capture regression exercises both public read APIs and verifies packet/read
ordering. It fails against pristine pre-fix sources (11 failures with the final
test), passes after the fix, and its two reads match PCSX2's 40-byte transcript
(two length headers plus 32 payload bytes). Linux `test_vulkan.py` now runs it.
All GPU/GS tests remain green. The Linux presenter also invokes the existing
race-clock capture scheduler; PCSX2 dump paths convert separators only on Windows.

Full Linux link: 4,580 objects, zero duplicates/other link errors, 23 existing SDK
stubs, no unresolved symbols after stub linkage. Fresh live captures at countdown
+120, GO+30 and GO+600 are in `native-race/`. GO+600 contains a real 64-byte read;
its 68-byte transcript (length header included) matches PCSX2. The two-field
race captures can still display a buffer inherited from the freeze, so zero
image error there alone is not independent raster evidence. The earlier eight-field
check and the independent retail GS stream are needed for that.

The live run reaches field 4,720 and exits through `DHD_BENCH_EXIT` without a crash:
fields 4,700–4,720 deliver 59.062 fields/s. This is a short loaded-race field
measurement, not a new claim about unique rendered FPS.

Independent retail PCSX2 boot completed the same NAV/seed and captured countdown,
GO+0.5 s and GO+10 s. Existing race window (five fields) and line correction
produce MAE 3.891, 7.312 and 3.489 respectively; these are **not** strict image
matches. Differences in game state/camera still contribute. Direct retail GS
stream comparisons are recorded separately below; no tolerance is widened.

## Bilinear filtering before alpha test

An independent-retail GO+30 stream exposed an interior-pixel difference at
GS (429,166), doubled to display y=332/333, maximum channel error 47. Native
draw 6,187 has vertices (35472,33621), (35609,33636), (35539,33650). The sample
lies strictly inside (edge values 742,583,1643; area 2968), so this is not the
documented coverage exception.

The shared shader truncated the 2D bilinear weighted sum only once. PCSX2's
software `lerp16_4` truncates the horizontal result before the vertical step.
For synthetic texel alphas [[64,65],[65,66]] at half/half, the old shader yields
65 rather than 64, incorrectly passing ATST GREATER 64. Both the literal sample
assertion and the discard assertion fail before the fix, and pass after it.
All existing GPU fixtures pass unchanged. This second fix removes the interior
retail-stream outlier; the complete stream's field 1 maximum falls 47 -> 10.
A separate doubled pixel in field 3 remains under investigation at this step.

## Sources

- [PCSX2 2.8.2 source](https://github.com/PCSX2/pcsx2/tree/v2.8.2), pinned at
  `fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3`: CMake build, GS runner CLI,
  software framebuffer export and synchronous `GSDumpReplayer` FIFO reads.
- [PCSX2 software scanline generator](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/SW/GSDrawScanlineCodeGenerator.all.h).
- Sony GS User's Manual v6.0, section 3.2.9 / figure 3-7,
  [archived manual](https://github.com/DarrenRainey/PS2-Programming-Docs/blob/master/GS_Users_Manual.pdf).
- [PCSX2 triangle scanline coverage](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/SW/GSRasterizer.cpp).

The public GPL PCSX2 source and its notices remain in the ignored build tree.
No third-party raster implementation is copied into the port.
