# Native renderer SPIR-V build (fork issue #14)

2026-10-25. Scope: compile and embed the native renderer's shaders on Linux;
creating graphics pipelines and enabling native rendering on Vulkan is separate work.
Matching `src/` and retail checks are unchanged.

## Sources

- [DXC SPIR-V documentation](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/SPIR-V.rst):
  register shifts, `vk::location`/`vk::index`, stage interfaces, DirectX layouts,
  `-fvk-invert-y`, position W and coordinate conventions.
- [Khronos HLSL guide](https://docs.vulkan.org/guide/latest/hlsl.html):
  DXC SPIR-V compilation and Vulkan shader interfaces.
- The available Vulkan MCP documentation search was consulted for DXC coordinates
  and dual-source blending; neither query returned a result. The primary sources
  above supplied the information. No external code was copied.

## Build and interface

`port/tools/vulkan_shaders.py`, called by the ordinary Linux `link.py`, now builds
all eight entry points used by `gpu_native.inc`: `native_vertex`, `native_pixel`,
`native_scanout`, `native_convert`, `present_vertex`, `present_pixel`,
`native_blit`, `native_upload_alpha`. Their `gs_spirv_<entry>` uint32 arrays share
`gs_spirv.h` with the existing exact compute modules. The native arrays have
`__attribute__((used))`: today's exact-only Linux backend does not reference them,
so ordinary optimisation would otherwise remove them. Generated files stay in build/.

Compiler remains the existing pinned DXC 1.9.2609 (archive/hash in the script).
Native command flags:

```
-spirv -T {vs,ps,cs}_6_0 -E <entry> -Gis
-fspv-target-env=vulkan1.2 -fvk-use-dx-layout -D DHD_NATIVE=1
-fvk-t-shift 16 0 -fvk-s-shift 48 0 -fvk-u-shift 64 0
-fvk-invert-y  # vertex stages only
```

All resources use set 0: bN -> binding N, tN -> 16+N, sN -> 48+N,
uN -> 64+N. These ranges cover the current declarations without collisions;
they are separate from the existing exact backend's layout. DXC removes unused
resources per entry point. Tests record the actual per-entry descriptor ABI,
including b1 Display, b3 Native, b4 NativeConvert, b5 NativeBlit.

Every emitted module, including the exact compute modules, must pass
`spirv-val --target-env vulkan1.2` before the generated header is written.
Missing spirv-val is a build error (install SPIRV-Tools). DXC's existing
`allow_uav_condition` warning on the exact shaders remains: that D3D hint is
ignored by SPIR-V; neither exact compute module changes bytes.

Native dual-source blending needs Location 0 / Index 0 and Location 0 / Index 1,
not two separate render targets. `__spirv__` guards `vk::location`/`vk::index`
attributes on `NativeOutput`; the D3D11 compiler still sees the original
SV_Target0/SV_Target1 semantics. A Vulkan native backend will need dualSrcBlend
and the corresponding SRC1 blend factors.

## Coordinates and limitations

The vertex shaders emit D3D-style clip Y. `-fvk-invert-y` negates their output Y,
for a Vulkan viewport with positive height, top-left framebuffer coordinates.
The future backend must not also flip the viewport. Depth remains [0,1] in both
APIs, with no OpenGL depth remapping. DXC emits OriginUpperLeft for fragments;
SV_Position.xy therefore continues to address top-left pixel centres (0.5,0.5).
The existing GS half-pixel/sprite handling stays in the shared HLSL. No extra
half-pixel adjustment is introduced. Fragment SV_Position.w differs between
D3D and Vulkan (W versus reciprocal W); these shaders only use position.xy,
and both vertex entry points emit W=1, so no W correction flag is needed.
Native noperspective varyings and the presentation UVs retain matching locations.
Rasterization/front-face state and visual parity need validation when a native
Vulkan pipeline exists; shader validation alone does not establish image parity.

## Validation

- `python port/tools/test_native_spirv.py`: all eight native modules validate;
  exact descriptor sets/bindings, dual-source indices, stage linkage, embedded
  bytes and retention in an optimised i686 object pass.
- `python port/tools/test_vulkan.py`: GS memory/GIF/transfer/CLUT, capture and exact
  GPU fixtures pass, zero failures on i686 RADV RENOIR. Initial sandbox execution
  hit SIGSYS; rerunning outside the sandbox passed.
- Rebuilt the parent's two exact compute shaders with the original generator:
  both new modules are byte-identical.
- Preprocessed parent/current `raster.hlsl` with `DHD_NATIVE=1` and without
  `__spirv__`: identical tokens (formatting differs), preserving the D3D11 input.
- Negative control: removing `used` from the generated header removes the native
  vertex module from the optimised i686 object, confirming the retention check
  detects the otherwise silent loss of unreferenced shaders.
- Ordinary `python port/tools/link.py`: successful Linux link, 4,580 objects,
  0 duplicate definitions, no remaining undefined symbols (23 SDK stubs reported
  by the link census). Read back build/port/link/dhd and found every one of the
  eight validated native SPIR-V files byte-for-byte in the executable;
  llvm-nm also lists all eight retained module symbols. This is a link/embedding
  check, not a new gameplay or image-equivalence claim.
