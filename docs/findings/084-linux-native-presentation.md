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
