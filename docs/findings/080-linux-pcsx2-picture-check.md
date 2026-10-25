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
