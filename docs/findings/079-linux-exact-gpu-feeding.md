# 079: Linux exact renderer: keeping the integrated GPU fed, and fiber stacks

2026-10-24/25, fork issue #1, following finding 078. Performance step plus one crash fix.
Host: i686 build, RADV RENOIR (integrated GPU, shared DDR4), 12 threads.

## Feedback loops

- `DHD_BENCH=first-last` (+ `DHD_TIMELINE=1`, `DHD_BENCH_EXIT=1`) now works in the Linux SDL
  presenter, with the same summary lines as Windows (fields/s, percentiles, per-thread CPU and
  waits by call site). In-process, no GDB. Menu: fields 420-1020 with the menu navigation of
  `bench_window_linux.py`; race: fields 3500-3800 with its race navigation. About 50 s and
  90 s; repeat runs agree within ~2%.
- `bench_exact_linux.py` replays (frame-001 menu, frame-009 race) for shader experiments, and
  all 12 local captures replayed through the session's starting `gpu.c` and the new one, final
  VRAM compared byte for byte, after every change. `test_vulkan.py` after every change.

## What the GPU was doing

`gpu_busy_percent` read ~87% at 1500 MHz while the GS worker had 12.7 ms CPU per field but
41 ms "busy": the GPU was the limit. Temporary counters (`[DEBUG-*]`, removed) found, per
menu field, 45.5 texture snapshots, each a full 4 MB VRAM copy (~182 MB read and written).
Placing VRAM/snapshot buffers in device-local memory instead of cached GTT was tried twice
and refuted: no GPU gain, slower CPU readbacks.

With the copies gone, shader-sensitivity variants (not exact, replay only) showed the per-pixel
work dominated but no stage did (texture -4 ms, writes -3, double depth -3 of ~16). The time
tracked the critical path, not the volume: each dispatch lasts as long as its longest tile
list, which one 8x8 workgroup walks in order (~3 us per primitive: dependent loads) while the
rest of the GPU idles. Summed longest list per dispatch: menu 15,375 / race 28,091 per 3
fields, though the menu had more tile memberships. Flushes came from the 256-primitive limit
(menu) and register-state changes (race). Prefetching the next primitive (registers, or 64 at
a time in group memory) made it slower and was dropped.

## Changes (each its own commit)

1. Texture snapshots copy only the pages in `texture_dirty`; every write to GS VRAM (draw,
   `upload_pages`, local copy) marks them, so the rest already equals the snapshot.
2. Up to 4096 primitives per dispatch (Linux's own `BATCH_DRAWS`; D3D11 keeps `MAX_DRAWS`).
3. `DHD_STATE_TABLE` (Vulkan only): primitives name their register state in `info.w`; the
   shader reads registers, palette and the independence flag from a table of up to 256 states.
   A state joins the batch only when both states pass `gs_raster_independent_pixels` and draw
   to the same FRAME/ZBUF layout with the same Z use, so each coordinate keeps its own words
   and the ordered tile lists still order each pixel.
4. Local transfers (TRXDIR 2) on the GPU with the shared `local_copy` entry (its b2 shifted to
   binding 10), as D3D11. Linux had no `local_copy`: each one waited for all drawing, copied
   on the CPU and uploaded again (twice a race field).
5. Deferred reads copy their pages on the GPU in order and `fetch_pages` waits only for that
   submission's fence, as D3D11 (the Linux version drained the GPU in `stage_pages`).
6. A command is submitted as soon as the other bank is done, not only at 16 commands: the
   GPU was ~56% busy in the menu while the worker waited 15 ms a field for a free bank.

## Fiber stacks (crash)

The race navigation crashed in 2/6 and 3/9 runs at field 1114 (SIGSEGV in `graphics_feed`,
fault address = stack pointer with its top nibble cleared). `func_002195F0` and `func_0019CB08`
build an image packet in a local array and pass it to `sceGsExecLoadImage`, whose GIF DMA uses
the address ANDed with 0x0FFFFFFF. On the PC that local is on the host fiber stack: Windows
puts those below 0x10000000, Linux's mmap put them at 0xE0000000+. Fiber stacks are now mapped
first fit between the PS2 RAM view and 0x10000000; the same navigation completed 12/12. The
session's starting build did 0/9 crashes and change 1 already 3/9; the mechanism doesn't
involve the GS backend, so change 1 most likely only changed the timing that reaches that path
(not proven). Interrupt handlers delivered from the idle loop still
run on the main thread's stack; no failure was seen there.

## Results

| DHD_BENCH (fields/s) | Start (e172a42) | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Menu 420-1020 | 20.06 | 25.44 | 33.13 | 32.34 | 32.13 | 31.50 | 40.35 |
| Race 3500-3800 | ~20 (finding 078) | | 33.3 | 41.9 | 45.3 | 47.3 | 57.8 |

The race was measured only from step 2 (its loop was built then). Menu after step 6: p50 24.4
ms, EE busy 3.9 ms, waits in `sceGsSyncPath` 11.5 ms and at vsync 9.1 ms; VU 12.5 ms and GS
11.8 ms CPU per field. The menu is now limited by VU/GS CPU work serialized by the game's
syncs, not the GPU (~56% busy before step 6). Replays (race / menu capture made at the end):
23.6 -> 9.8 and 29.6 -> 13.2 ms/field.

## Validation

- All 12 earlier captures and the two new ones keep byte-identical final VRAM against the
  starting backend after every step. None of these dumps records a read, so they say nothing
  about readbacks.
- `test_vulkan.py`: 0 failures. New fixtures, each red without its rule and green with it:
  `aliased_states` (two independent states whose frames alias across tiles; without the layout
  check: fails 3/3, and 10 of 12 captures differ), `deferred_read_waits` (settle right after
  4000 queued sprites; without the fence wait: fails 3/3). `local_copies` now expects GPU
  copies on Linux as on Windows (a truncated dispatch fails three copies). Copying only the
  first dirty run of a snapshot: 19,741 failures.
- Captured race (field 3600) and menu (700) pictures look right. `src/` and the matching checks
  are unchanged; D3D11 builds the shader with the new defines off (not built or run here).

## Left

Menu below 60 fields/s; the race at 57.8 is close. Next candidates: GS/VIF CPU work
(GIF decoding, `draw`, CPU binning in `flush`) and issue #11 (`-mstackrealign` on hot host
code). The StoreImage/`sceGsSyncPath` drains are game-visible synchronization shared with
Windows and were left as they are.

## Sources

Khronos Vulkan specification on [command buffer submission and fences](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html)
and [synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
(`vkGetFenceStatus`, transfer/compute barriers); the repository's D3D11 backend for
`local_copy` and the asynchronous `stage_pages`/`fetch_pages` design. No external code was copied.

## Pinned DXC

The builds above used a DXC extracted by hand into `/tmp`, which nothing in the repository
recreated. `port/tools/vulkan_shaders.py` now fetches the official Linux release v1.9.2609
(`linux_dxc_2026_09_28.x86_x64.tar.gz`, SHA-256 `96faadc7…aa39a1`, the digest GitHub publishes
for that asset) into `build/downloads`, rejects any other hash and unpacks it under
`build/port/dxc`, as `sdl3.py` does with SDL. `DXC=path` still overrides it; with no network and
no download a `dxc` on PATH is used with a warning. The binary is not committed (tens of MB,
platform-specific; the repository holds sources). The pinned compiler's SPIR-V header is
byte-identical to the hand-extracted one's; `link.py` and `test_vulkan.py` pass with `DXC`
unset.
