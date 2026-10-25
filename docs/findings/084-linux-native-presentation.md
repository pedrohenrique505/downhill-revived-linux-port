# Linux native window and interpolation (fork issue #16)

2026-10-25. Implementation in progress. Matching sources/checks unchanged.

## Sources

- Vulkan MCP `search_vulkan_docs` and `get_vulkan_topic` (synchronization).
- [Khronos swapchain semaphore reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html):
  presentation semaphores belong to acquired images; submission fences alone do
  not establish their presentation lifetime.
- [SDL Vulkan surface creation](https://wiki.libsdl.org/SDL3/SDL_Vulkan_CreateSurface):
  window flag, required instance extensions, surface lifetime.
- The project's GPL `native.c` and `gpu_native.inc` provide the existing render
  thread, field handoff, interpolation, movie and presentation semantics.
  No external code copied.

## Step 1: swapchain backend

Optional SDL window on the native Vulkan device, graphics/compute/present queue
selection, UNORM swapchain, transfer scanout with point/linear filtering,
aspect/letterbox and player quadrant selection. Render-thread acquire/submit/
present uses a fenced acquire semaphore and one completion semaphore per image.
Zero-size windows skip presentation, out-of-date/suboptimal surfaces recreate,
and resize/shutdown drain work before releasing swapchain resources. Window size
is published by the SDL thread; render code makes no SDL window calls.

Offscreen transfer/backend fixtures: 17 PASS with the local i686 Khronos API and
synchronization validation layer (zero validation messages). Evidence:
`/tmp/issue16-test-validation.log`. Sandbox execution hits driver SIGSYS, so GPU
checks run outside the sandbox. Window integration and acceptance remain pending.

## Step 2: shared frontend and SDL integration

Linux now selects native before any game packet is recorded, retains exact for
Z probes, and hands fields/movies to the existing native render thread. Settings,
scaled screenshots, race-clock/field captures and native benchmark counters use
the shared frontend. Window events stay on the EE thread. The graphics worker
shuts down before the renderer; presentation joins before destroying the window.
DHD_DUMP requests an EE/kernel snapshot at the next safe kernel boundary (it does
not suspend an arbitrarily hung instruction).

Synthetic SDL window tests pass for ordinary 60 Hz fields and 120/144 Hz timed
interpolation, including movie readback, point/aspect options, minimize/restore,
resize and teardown. First validation run caught an acquire-to-layout-transition
hazard: the transition's source stage now includes TRANSFER, matching the acquire
semaphore wait. The unchanged strict validation check then passes every mode
with zero warnings/errors. Results: 150 game fields each, 150/283/344 presented
pictures, 0/267/324 interpolated pictures. Rendered totals can advance by one
before shutdown. Evidence: `build/port/issue16/window/`. These are synthetic
pictures, not full-race acceptance. Ordinary game link/live checks are pending.

## Capture boundary and regression checks

Capture display dimensions now come from the raw completed picture, independently
of `DHD_SCREENSHOT_WINDOW`. A movie fixture checks formatted 320x200 screenshots
alongside raw 32x24 capture size/content; all three window modes pass strict
validation again (150 fields; 150/296/355 presents). The ordinary Linux link
completes with its existing 23 SDK stubs and no unresolved symbols. All eight
SPIR-V modules/stage/embedding checks and the portable-frontend header check pass.
All 23 validated replays (86 fields) retain exactly the previous issue-15 RGBA
hashes, FIFO readbacks and profile counters. Evidence: `build/port/issue16/replay/`,
`issue16-replay.log`, `issue16-spirv.log`, `issue16-portable.log` under build/port/.

First live race uses the native window, scaled screenshots, DHD_DUMP, benchmark
and timeline successfully. Actual window and internal pictures inspected after
resize. Its GDB observer stops every field and reports only 56.54 fields/s at
6000-6600; this is retained as an instrumented result, not a 60-Hz performance
claim. Full-race completion and unpaused 120/144 Hz checks are still running.
