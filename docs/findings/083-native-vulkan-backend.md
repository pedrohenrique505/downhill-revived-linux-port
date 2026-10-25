# Native Vulkan graphics backend (fork issue #15)

2026-10-25. Offscreen implementation and acceptance checks complete. Native drawing uses a separate Vulkan graphics/compute
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

## Initial tooling pause (resolved below)

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

## Offscreen image comparison and integration

A fresh D3D11 run (Wine/DXVK, same prefix/compiler as finding 081) and Vulkan run
with all 23 accepted version-8 captures saved every field (--all-pictures).
The same 86 fields at scale 2 give 84 identical full-RGBA FNV hashes. The other
two fields (retail-normalized/gs-g+30 field 5 and gs-g+600 field 1) each differ
in only two RGB pixels by one channel unit: RGB MAE 0.000000726609003, maximum
channel error 1, no pixel above 16. All 23 FIFO/readback transcripts and all
frontend profile counters match. Pictures are compared at their original size,
without masks, shifts or excluded pixels. This meets the recorded-field image
comparison; it is Windows API code run through DXVK, not a physical Windows GPU.
`compare_native_pictures.py` writes full per-field metrics and checks readbacks;
its documented default bounds are MAE 0.1 and changed-pixel fraction 0.001.
The measured differences are far below those bounds.

Evidence (ignored): build/port/issue15/d3d-all-pictures/,
vulkan-all-pictures/ and picture-comparison.json. Every Vulkan replay had explicit
Khronos and synchronization validation enabled, zero API/undefined-value warnings
or errors. The native GPU and both devices are destroyed before process exit.

The shared 13 transfer fixtures pass Vulkan validation, plus backend fixtures for
mixed-scale blits, overlapping self-copy snapshots, surface growth preserving old
colour/clearing new pixels, reversed depth and masked target feedback. The existing
GS memory/GIF/capture/exact GPU suite passes; native.c still compiles with all GPU
headers poisoned. Native frontend teardown now releases frame/log storage after
joining the worker and retiring native GPU work.

Ordinary Linux link includes native.c/native_gpu.c and hooks backend destruction;
texture packs and sharp font remain unavailable on Linux. The link completes with
4,582 objects, no duplicates/remaining undefined symbols and the existing 23 SDK
stubs. The window presenter still selects exact: this issue supplies the explicit
gs_vulkan_native() offscreen API; swapchain/window presentation is a separate step.

Additional scale checks: all 23 captures (86 fields) pass API/synchronization
validation at scale 1 and scale 4 after the final resource cleanup. Their FIFO
transcripts match the existing D3D11 references. RGBA hashes agree for 84/86 at
1x and 69/86 at 4x. The reference has only the first three PPMs per capture
(58 total): all those RGB pictures are identical at 1x; differing 4x pictures
have maximum channel error 1, worst RGB MAE 0.000000290643601. The remaining
hash differences at these extra scales have no full-picture score claimed;
the full 86-picture acceptance comparison is at 2x. Raw summaries/partial
scale scores: build/port/issue15/scales-comparison.json.

The two exact compute modules remain byte-identical to their pre-change builds
(SHA-256 compared). No retail code or strict image/decompilation checks changed.
No game data or pictures are committed. No live native Linux window performance,
presentation, texture pack/sharp font support or physical-Windows parity is claimed.

Reproduce with a configured i686 Vulkan validation layer:

```
VK_LAYER_PATH=/path/to/local/manifests python port/tools/test_native_transfers.py --vulkan --validation
VK_LAYER_PATH=/path/to/local/manifests python port/tools/test_native_replay.py build/port/vk FIELD.gs --vulkan --validation --all-pictures
WINEPREFIX=/path/to/dxvk/prefix python port/tools/test_native_replay.py build/port/d3d FIELD.gs --all-pictures
python port/tools/compare_native_pictures.py build/port/d3d build/port/vk build/port/comparison.json
```

## Checkout re-check

Same day, on a clean checkout at 149816e, with the local Khronos layer: the 17 transfer/backend
fixtures pass with `--vulkan --validation`. All 23 captures (86 fields, scale 2) replay with
validation enabled in every run and no VUIDs, validation warnings or errors. Against the D3D11
references: 84/86 identical RGBA hashes, the same two fields at two pixels each (MAE 7.27e-7,
max 1), 23 matching FIFO transcripts. The Linux link completes with 23 stubs and nothing left
undefined. `src/`, `config/` and `tools/` are unchanged since a0ce095.
Evidence (ignored): build/port/issue15/recheck-vk/, recheck-comparison.json, recheck-link.log.
