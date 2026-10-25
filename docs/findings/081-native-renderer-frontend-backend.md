# 081: The native renderer split into a portable frontend and a D3D11 backend

2026-10-25, fork [issue #13](https://github.com/pedrohenrique505/downhill-revived/issues/13), part of
[#5](https://github.com/pedrohenrique505/downhill-revived/issues/5) (the native renderer on Linux,
Vulkan). No change to `src/` or the matching checks.

## The split

`port/gs/gpu_native.inc` (2,958 lines) held both the renderer and Direct3D 11. It is now:

- `port/gs/native.h`: the shared types (surfaces, commands, frames, the probe log, statistics,
  `Native`) and the two interfaces below. It includes `<windows.h>` for the locks (the port's own
  on Linux) and no graphics header.
- `port/gs/native.c` (frontend, a translation unit of its own): surface and page ownership,
  texture decoding and its cache, GS state to draw state, command and vertex recording, the exact
  Z probe (watch, log, replay, band, exact targets), the GS hooks, local copies, the vsync handoff,
  the render and interpolation threads, segment pairing and the interpolated vertices, surface
  sizing and the resolution scale, pacing, diagnostics (`DHD_NATIVE_*`, `DHD_FIELD_TRACE`,
  `DHD_INTERP_DUMP`, the exit profile) and `gs_native_*` except `gs_native_vsync`.
- `port/gs/gpu_native.inc` (873 lines, still included by `gpu.c`, whose shader cache it uses):
  the Direct3D 11 backend. Blend and depth state objects, surface textures and views, blits,
  copies, alpha uploads, texture conversions, the draw loop, scanout, the swap chain, GPU
  timestamps, shader set-up, texture-pack samplers.

The backend implements `native_gpu_*` (native.h): make a decoded texture's image and view (with the
sharp font's), free them, a texture pack's view (GS worker); create or grow a surface's image, run
a field's commands, scan out (or show a movie picture), present, read the picture back, wait for
the frame latency, set the present filter, GPU timestamps, the window's height (render thread).
The frontend's half for backends is `native_create`, `native_start`, `native_vsync`,
`native_present_options`, `native_viewport`, `native_page_inverse`, `native_capture` and
`native_profile`.

The frontend handles a backend's objects only as opaque `NativeImage`/`NativeView` pointers
(textures, texture-pack replacements, the sharp font). Each surface's GPU objects live in the
backend (`NativeD3d.surfaces[]`, parallel to `Native.surfaces[]`); `NativeSurface.size` (0 until
there is an image) tells the frontend whether one exists. `replace_samplers[7] != NULL`, which the
frontend used to ask whether replacements can be drawn, became `Native.replacements`, set where
the samplers are made.

The exact backend is reached through `NativeHost`, the first member of its state (`Gpu.host` in
`gpu.c`): the frontend finds itself there and writes the probe clip. Its draw and staged reads are
the callbacks the frontend takes over in `native_start` (`g->draw`, `g->stage_pages`), and its
range sync is `g->sync_range`, so the frontend no longer calls `gpu.c`'s static functions. The
Linux exact backend (`port/platform/linux/gpu.c`) can host the frontend by starting its state with
a `NativeHost`.

`texpack.h` and `font_sharp.h` name `ID3D11Device` and `ID3D11ShaderResourceView` as struct types
instead of including `<d3d11.h>`, so the frontend can use their GS-worker functions.

The frontend also builds on Linux (not linked yet: `link.py` keeps `gs/native.c` in
`LINUX_LATER` until there is a Vulkan backend). For it the port's Linux headers gained
`YieldProcessor`, `_BitScanForward`, the `.bmp` headers, and waitable timers that report none (a
caller sleeps instead: `DHD_INTERP_HZ` pacing).
`port/tools/check_native_portable.py` compiles `native.c` as `link.py` does with every Direct3D,
DXGI and Vulkan header replaced by an `#error`; a file that includes one fails it.

## Checking the D3D11 renderer without Windows

The D3D11 backend runs under Wine with [DXVK](https://github.com/doitsujin/dxvk) 2.5.3 (Wine's own
D3D11 has no double-precision shaders, which the exact backend requires) and Microsoft's
`d3dcompiler_47.dll` (Wine's HLSL compiler rejects `raster.hlsl`: "Method 'Load' is not defined on
type 'RWByteAddressBuffer'"). The 32-bit DLL is the one winetricks installs, from Mozilla's Firefox
62.0.3 Windows installer (`core/d3dcompiler_47.dll`). Both go into the Wine prefix only; nothing
of them is in the repository. Programs are built with `winegcc -m32 -b i686-windows` against
Wine's import libraries and headers; the same headers compile `gpu.c`, `texpack.c` and
`font_sharp.c` for the Windows target.

`port/tools/test_native_replay.py` replays version-8 GS captures (`port/gs/replay.c`'s format)
through `DHD_RENDER=native` and writes each field's picture hash, the readbacks' hash and the
exit profile counters; `--expect` compares with an earlier run, `--tree` builds another
checkout's renderer (a worktree of the commit before the split), `--env` sets renderer options.
The 29 local captures (`build/port/issue1-picture`, from finding 080; six are PCSX2 dumps and are
refused by the replay on both sides) give 86 pictures: race, riding, menu, title, the triangle
fixtures and readback captures. Two runs of the same build give the same hashes.

Results, the commit before the split (`ce65794`) against the split:

| options | pictures | readbacks and profile counters |
| --- | --- | --- |
| defaults (scale 2) | 86 identical | identical |
| `DHD_SCALE=1` | 86 identical | identical |
| `DHD_SCALE=4` | 86 identical | identical |
| `DHD_SCALE=window` (no window: scale 1) | 86 identical | identical |
| `DHD_NATIVE_SURFACES`, `DHD_NATIVE_PROBES`, `DHD_NATIVE_TEXLOG`, `DHD_NATIVE_TRACE` | 86 identical | identical |
| `DHD_INTERP=1` | 86 identical | identical except `interp_frames` and `conversions_run` |

With `DHD_INTERP=1` the number of in-between pictures depends on wall time, so those two counters
differ between two runs of the same build too; the pictures the replay reads back do not.

`test_native_transfers.c` (13 synthetic upload/copy checks) passes before and after under Wine.

## Checked with a Windows dhd.exe

`dhd.exe` is cross-built on Linux: `DHD_PLATFORM=windows` takes the MSVC x86 libraries and the
Windows SDK from an [xwin](https://github.com/Jake-Shadle/xwin) 0.10.0 splat (`pctarget.py` sets
`INCLUDE` and `LIB`, as `vcvarsall.bat` does), and SDL3 is built with CMake's Windows cross
settings and LLVM's `lld-link`, `llvm-ar`, `llvm-rc` and `llvm-mt` (`sdl3.py`). Two worktrees, the
commit before the split (`ce65794`, with the cross-build change applied) and after (`7fbae06`),
each built with `build.py` + `link.py` (`DHD_BUILD_VU_AOT=1`, as `build_clean.py`): 3,815 units, no
errors, 23 SDK stubs as before.

They run under Wine 11.20 in a prefix of their own with DXVK 2.5.3 (SHA-256 as winetricks pins it)
and the 32-bit `d3dcompiler_47.dll` winetricks now takes from Microsoft's Windows SDK installer
(`2630bae9681db6a9f6722366f47d055c.cab`). That compiler doesn't know
`RasterizerOrderedByteAddressBuffer`, so the exact renderer's hardware coverage bridge is off and
its reference compute path draws (the same in both builds). The race gates
(`race_compare.launch_port`, `gate_gs_optimization.py`'s menu run) start a `.exe` through `wine`
off Windows (`compare_pcsx2.port_command`).

- `gate_native_renderer.py`: before (`--record`, the reference) and after both pass. EE state matches
  the reference at GO+0/2/12/16, the arms match each other through GO+4500, and all 1,778
  readbacks match through field 9006. The 12 race pictures (6 moments, native and exact arms) are
  byte-identical before and after. The profile counters agree except for those that grow with
  the fields shown after the last capture: the gate stops the game on wall time (8,531 against
  8,575 presented).
- `test_native_paths.py` (new): runs the game windowed (DXGI flip-model swap chain and its
  presentation) with `DHD_RENDER=native` and the frame clock, and hashes the pictures saved at
  fixed fields. Covers the two silent logo movies (the movie picture through the native scanout),
  the title and main menu in the sharp font and in `font = original`, and the same menus with
  a texture pack. The pack holds one 2x checker per texture the first menus draw (34), so the
  replacements' loader threads, WIC decode and samplers are exercised. Before and after give the
  same 10 pictures; two runs of one build give the same.
  The movies with sound aren't hashed: their picture follows the sound mixer's clock
  (`movie_audio`), which moves with wall time, so two runs of the same build differ there.

## Not checked here

- Real Windows (the checks above ran under Wine and DXVK, not Microsoft's D3D11 driver stack),
  exclusive fullscreen, and the movies with sound.
- `port/tools/test_hw_renderer.py` and `test_runtime.py`'s `test_gpu` build `gpu.c` without the
  native renderer's other symbols (texpack, host, pacing); they failed to link before the split
  too (checked at `ce65794` with winegcc) and are unchanged.
