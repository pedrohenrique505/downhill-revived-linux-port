# 076: Linux exact GS rasterization through Vulkan compute

2026-10-24, fork issue [#1](https://github.com/pedrohenrique505/downhill-revived/issues/1),
following the missing-backend diagnosis in finding 075.

## Implementation

The Linux link now substitutes `port/platform/linux/gpu.c` for the D3D11 backend.
`kernel_init` initializes it immediately after `gs_reset`, before GIF submission;
initialization failure is fatal and logged instead of silently drawing nothing.
DXC compiles the existing `raster.hlsl` `main` entry point to SPIR-V at build time.
Explicit bindings are b0=0, u0=1, t0=2, t1=3, t2=4. Vulkan 1.2, a compute queue,
Float64, strict float32 signed-zero/Inf/NaN support and resource/dispatch limits
are checked. Shared C layouts have compile-time assertions (State=3248 bytes,
Primitive=208 bytes, depth offset=176). `spirv-val --target-env vulkan1.2` passes.

The backend keeps 4 MB GS VRAM, ordered 8x8 tile lists, register/palette state,
texture-page invalidation and feedback snapshots. Each compute batch finishes
before mapped buffers are reused. CPU writes upload dirty pages; readbacks merge
only GPU-written pages. CPU writes first synchronize previous GPU writes, and
host-transfer flushes preserve command ordering. Deferred read snapshots retain
captured pages until settlement. Local copies retain the existing CPU path,
including packed formats, overlapping rectangles, wrapping and scan direction.
SDL uses its Vulkan renderer for presentation and synchronized CPU GS scanout,
so capture/readback sees the same GS storage. Host-cached coherent allocations
are preferred to avoid slow CPU reads of uncached device-local mappings.

The shared shader now checks line endpoints in integer GS coordinates before
division. The old float quotient let Vulkan include the excluded endpoint:
`test_gpu.c` expected a blank pixel at (9,55), but got 0x80443322. The assertion
was kept; the integer half-open range makes the same test pass. No tests or
retail-byte matching checks were weakened. `src/` is unchanged.

The controller worker previously removed all SDL events, discarding window-close
and text events. On Linux the window thread now pumps video, and window/controller
consumers remove disjoint event ranges using thread-safe `SDL_PeepEvents`.
Windows retains its original controller event loop.

## Validation so far

- Agreed test seam: public GS register/GIF/VRAM interfaces, with independent
  expected pixels from the existing D3D11 WARP fixtures.
- Baseline field-600 capture: 0 nonblack pixels (assertion fails).
- Shared exact raster fixtures on AMD RADV RENOIR, i686: 0 failures. Covers
  coverage, Z32 precision, textures, mipmaps, feedback, alpha/fog/blend,
  packed targets, batching order, local transfers and deferred reads.
- GS memory/GIF/CLUT suite: 3,000 uploads, 6,657,394 pixels, 0 differing.
- Host C builds, Python byte compilation and `git diff --check` pass.
- Full Linux link: 4,580 objects, 0 duplicate definitions, 23 existing SDK stubs,
  no unresolved symbols after stub linkage. Changed runtime objects relinked.
- Windowed Wayland/SDL Vulkan scanout displays the loading card (field 120,
  179,008 nonblack pixels), rider selection (field 600) and subsequent menus.
  Evidence: `/tmp/dhd-issue1-vulkan-final/field-120.png`,
  `/tmp/dhd-issue1-vulkan-cached/field-600.png`, and
  `/tmp/dhd-issue1-vulkan-race/field-600.png`. All captures/dumps stay untracked.
- SIGINT terminates the windowed process in approximately 64 ms (Python signal;
  physical terminal Ctrl+C has not been exercised).
- This host initially had only the 64-bit RADV driver; Vulkan i686 initialization
  returned -9. Isolated packages in `/tmp` enabled the initial tests. The user
  installed `lib32-vulkan-radeon` for subsequent ordinary launches.

## Windowed acceptance results

The ordinary system-driver run reached race phase 2 (GO), then phase 3; the
race clock advanced normally and new race captures were produced through field
4442. The user physically verified keyboard pedal/steering/pause and Xbox
acceleration/steering, reporting that both work. Menu navigation before the race
used the existing public pad script, not forced game-state writes. The script
was disabled for physical checks. Evidence includes
`/tmp/dhd-issue1-vulkan-accept/field-3002.png` (starting gate) and the later
`frame-008/009-native-002.bmp` race captures in that directory.

Three compositor close requests targeted exclusively the spawned test window:
all exited normally, in 0.175, 0.126 and 0.126 s. A subsequent ordinary-driver
SIGINT test exited in 0.064 s. Logs and owned-window identities are under
`/tmp/dhd-issue1-vulkan-close/`; the harness/results are
`/tmp/dhd-vulkan-close.py` and `/tmp/dhd-vulkan-close-results.log`. This verifies
the SDL close-request path with the controller worker active. Physical terminal
Ctrl+C was not separately pressed.

Active rendering is slow. Under GDB, menu fields 420..600 took 22.037 s:
180 VSync fields, 180 menu-frame increments and 180 display-buffer changes,
about 8.17/s for each. This explicitly measures menu frames as well as fields.
During racing, fields 3540..3660 took 10.431 s: 120 fields, 120 buffer changes
and 120 race-clock ticks (11.50/s). The menu frame counter stops in a race, so
it is not a racing FPS counter; clock ticks and buffer changes are reported as
such, without claiming independent unique rendered-game-frame counts. These
are monitored samples on a shared host, not a clean performance benchmark.
Startup's near-60 fields/s with no rasterization in finding 075 is not a valid
rendering-performance baseline. No optimization/FPS guarantee is made.

Official PCSX2 2.8.2 software GS replay independently rendered two fields after
the inherited initial freeze for menu and race captures. Correct circuit/base
and half-height scanout were checked using the unchanged `read_dump` algorithm
from `compare_pcsx2.py`; no alignment search or tolerance change was applied.

| Last replayed field | Pixels | RGB MAE | Max channel error | Nonidentical pixels | Pixels with error >16 |
| --- | ---: | ---: | ---: | ---: | ---: |
| menu 602 | 286720 | 0.3684756324 | 5 | 184626 | 0 |
| starting gate 3002 | 229376 | 0.3807867141 | 62 | 144758 | 2 |

Reference/Vulkan images and JSON metrics are in `/tmp/dhd-pcsx2-replay/` and
`/tmp/dhd-pcsx2-race-replay/`. The images are close but **not pixel-identical**;
the two race outliers remain for cross-backend investigation. Replay isolates
the GS path; it is not independent retail game boot/physics validation. No
D3D11 production A/B was possible here (no Windows SDK/build environment).
Synthetic tests compare independent literals shared with D3D11 fixtures, not
an executed D3D11 game reference. Retail boot equivalence, D3D11 game/readback
parity and the race pixel outliers remain open; issue #1 is not fully accepted.

## Review

Two independent code-review agents reviewed standards and issue requirements
against baseline `adb4a22a43582fcf08f7d05a23945da9073a7ea5`.

### Standards

No documented-standard violations. The initial duplicated texture-page,
independence and depth calculations were extracted into `raster_math.h`, and
the reviewer confirmed the operations/conditions match the original D3D11
implementation. Final review: 0 pending findings.

### Spec

The review found that an unsuitable first Vulkan physical device prevented
trying a later suitable device. All capability checks now run per candidate;
the reviewer confirmed that correction. Remaining findings are acceptance
items described above, not a claim that exactness has been established.

## Sources consulted

- [DXC SPIR-V documentation](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/SPIR-V.rst):
  HLSL buffer layout, explicit register shifts, target environment.
- [Vulkan compute guide](https://docs.vulkan.org/guide/latest/compute_shaders.html)
  and [synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html):
  buffer barriers, host-coherent reads and submission completion.
- [Vulkan specification](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html):
  Float64 and float-control capabilities and device limits.
- [SDL_PeepEvents](https://wiki.libsdl.org/SDL3/SDL_PeepEvents) and
  [SDL_PumpEvents](https://wiki.libsdl.org/SDL3/SDL_PumpEvents): event ranges,
  thread-safe queue access and main-thread video pumping.
- [Official PCSX2 releases](https://github.com/PCSX2/pcsx2/releases) and
  [running PCSX2](https://pcsx2.net/docs/setup/running/): isolated reference runtime.

No external implementation was copied. Host batching/depth/snapshot algorithms
are adapted from this repository's D3D11 renderer, using the same shader.
