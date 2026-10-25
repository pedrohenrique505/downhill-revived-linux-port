# Native Vulkan graphics backend (fork issue #15)

2026-10-25. In progress. Native drawing uses a separate Vulkan graphics/compute
queue/device from the exact Z-probe renderer. Matching sources/checks are unchanged.
Window/swapchain integration is outside this offscreen backend step.

## Sources

- Vulkan MCP `search_vulkan_docs` (VkGraphicsPipelineCreateInfo, dualSrcBlend) and
  `get_vulkan_topic` (synchronization) returned Vulkan documentation successfully.
- [Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
  and [render passes](https://docs.vulkan.org/spec/latest/chapters/renderpass.html):
  attachment dependencies, image layouts and command/resource lifetimes.
- Existing GPL project backend `port/gs/gpu_native.inc` supplies the intended
  surface, blending, depth, conversion and scanout semantics. No external code copied.

## Implementation steps

1. Native Vulkan resource infrastructure: scaled colour/D32 surfaces, immutable
   decoded texture uploads, fenced upload storage and deferred destruction.
   Checked with clang's i686 syntax check. Not yet linked or image-validated.
