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

## Sources

- [PCSX2 2.8.2 source](https://github.com/PCSX2/pcsx2/tree/v2.8.2), pinned at
  `fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3`: CMake build, GS runner CLI,
  software framebuffer export and synchronous `GSDumpReplayer` FIFO reads.
- [PCSX2 software scanline generator](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/SW/GSDrawScanlineCodeGenerator.all.h).

The public GPL PCSX2 source and its notices remain in the ignored build tree.
No third-party raster implementation is copied into the port.
