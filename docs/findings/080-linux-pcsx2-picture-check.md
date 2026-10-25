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

## Sources

- [PCSX2 2.8.2 source](https://github.com/PCSX2/pcsx2/tree/v2.8.2), pinned at
  `fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3`: CMake build, GS runner CLI,
  software framebuffer export and synchronous `GSDumpReplayer` FIFO reads.
- [PCSX2 software scanline generator](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/SW/GSDrawScanlineCodeGenerator.all.h).

The public GPL PCSX2 source and its notices remain in the ignored build tree.
No third-party raster implementation is copied into the port.
