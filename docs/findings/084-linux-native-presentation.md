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

## Live-race harness and context fairness

The first 60-Hz race reaches the real finish flag at official tick 23551
(392.5167 seconds), matching the prior full-pedal fixture. Its GDB callback then
stalls on an inferior function call; all game threads are ptrace-stopped. The
closing portion is invalid and that own test group was stopped. The replacement
observer reads only its child's `/proc/PID/mem`, without stopping any thread.

The replacement's 60-Hz bounded run exercises real resize, screenshots, DHD_DUMP,
benchmark and timeline. It closes cleanly with exit 0 on WM_DELETE_WINDOW.
The compositor ignores xdotool's root-window `_NET_CLOSE_WINDOW` request; the
harness now sends the standard WM_PROTOCOLS/WM_DELETE_WINDOW client message to
its own verified application window. Protocol/layout references: installed
Xlib.h and SDL_x11events.c, and
[xdotool's request implementation](https://github.com/jordansissel/xdotool/blob/master/xdo.c).
No external implementation copied. Raw evidence: `race60/`, `race60-unpaused/`.
Unpaused benchmark: 57.136 fields/s, 57.15 pictures/s at fields 6000-6600.

An added display-vsync interpolation fixture (DHD_INTERP=1 without an explicit
rate) times out before the fairness fix. GDB stacks show the main thread inside
gs_native_present_options/pthread_mutex_lock while the render thread repeatedly
presents under the context lock. The first live 120-Hz run also stalls logical
progress while continuing to present: 3459 fields versus 38085 pictures. It was
closed and is not full-race acceptance.

Linux context clients now publish requests before waiting; the render thread
lets those requests acquire the mutex before starting another picture. Windows
retains its original lock behaviour. All four window modes pass the unchanged
API/synchronization checks after this fix, including the previously timed-out
vsync case. Scanout and presentation additionally share one submission on a
window, removing a CPU fence wait for the current field; skipped presentations
still submit the field's work. Offscreen submissions are unchanged.
Evidence: `issue16-vsync-stacks.log`, `window-fair/`, `window-submit/`.

## Picture-rate reporting after benchmark reset

The first corrected 120-Hz full race finishes at official tick 23551, includes
600 further fields (the normal result transition), resizes twice and closes with
exit 0. Benchmark fields 6000-6600: 53.721 game fields/s and 120.10 pictures/s,
1319 interpolated pictures. Wall duration 518.93 s includes boot/loading. API
validation was off for this timing run; the separate window/replay fixtures have
strict validation on. Evidence: `race120-final/result.json` and `run.log`.

DHD_BENCH resets interval timestamps, but the exit profile used lifetime picture
counts as its numerator, inflating the reported rate. It now uses the interval
count from the same epoch as the timestamps. A fixture observes the first
post-reset picture's count and wall time independently, and compares the report
within 5% (includes polling/readback latency). The old formula reports 137.72
while the independent rate is 109.99 (FAIL); corrected report 111.54 versus
111.11 (PASS). This checks accuracy under load rather than assuming the GPU can
always reach the configured rate. All four API/synchronization window modes
pass again. Evidence: `window-profile-final/`,
`issue16-profile-red-green-final.log`. Timing changes do not alter rendering.
