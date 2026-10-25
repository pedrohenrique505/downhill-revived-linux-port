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

2. Graphics pipeline cache keyed by blend/depth/mask/shader kind; dual-source
   alpha, fixed blend factors and reversed GS depth comparisons. Main draws,
   scaled blits, overlapping copies via snapshots, masked alpha upload,
   surface-texture conversion and display scanout now execute on Vulkan.
   The exact backend reserves NativeHost as its first member and honours its
   inclusive probe clip. Explicit gs_vulkan_native() attaches the frontend;
   ordinary game/window selection is not enabled by this offscreen step.

3. `test_native_replay.py --vulkan` builds an i686 Linux replay of the same
   version-8 capture used by the Windows test. It saves stderr, fails on replay
   errors and validation VUIDs, and leaves all game-derived pictures in build/.
   First actual RADV RENOIR replay of local menu/frame-000.gs: three fields,
   1280x896, 102 commands, 19 decoded textures, 144 imports, 2 conversions.
   Hashes (not yet compared against D3D11): c561c73e1fba1fb5,
   55f844468a10c895, 0ae544c7161daec1. No FIFO reads in this capture.
   Evidence: build/port/issue15/first/. The sandbox driver run hit SIGSYS;
   the same replay completed outside the sandbox.

## Pending tooling and acceptance

The host has no VkLayer_khronos_validation.json or
libVkLayer_khronos_validation.so in /usr, /opt, /tmp, ~/.local or build/port.
The issue explicitly requires no validation-layer errors: an unvalidated replay
cannot establish this. Paused before installing extra tooling, as the user
requested. Need a working 32-bit VK_LAYER_KHRONOS_validation (including its
manifest/dependencies). Windows image difference measurements, broader synthetic
coverage, ordinary link integration, resource/validation audit and race Z-read
parity still need completion. No issue acceptance or production readiness claimed.

## Local validation tooling and storage formats

Resumed with permission to install tooling without sudo. Arch packages
lib32-vulkan-validation-layers 1.4.363.0-1 and the matching 64-bit package
(manifest only) were extracted under build/port/issue15/layers; no system files
were changed. VK_LAYER_PATH points at the extracted manifest, whose library path
points to the local i686 .so. ldd reports all runtime dependencies available.

Explicit --validation now requires the Khronos layer on both devices and enables
VK_LAYER_VALIDATE_SYNC=1. The native debug messenger and both stdout/stderr are
checked: warnings, errors and VUIDs fail the replay (including undefined-value
warnings). The initial environment-only check missed stdout warnings; its result
was insufficient. The stricter test caught screen/native_converted declared as
Rgba32f but bound to RGBA8_UNORM views. DXC-specific image_format("rgba8")
attributes fix this, guarded so D3D11 sees unchanged declarations. Reference:
[DXC SPIR-V image formats](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/SPIR-V.rst).
The SPIR-V format regression failed before the fix and passes after it.
Three menu fields now run with explicit validation plus synchronization checks,
zero warnings/errors, and retain the D3D11 reference's exact RGBA hashes.
