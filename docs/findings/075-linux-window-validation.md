# 075: Linux window validation exposes a missing raster backend

2026-10-24, branch `linux-port`, executable built at `8ce1738`.
Spec: [fork issue #1](https://github.com/pedrohenrique505/downhill-revived/issues/1),
“Linux: check the exact renderer with a window (picture, speed, input, closing)”.
Confirmed failure: [#12](https://github.com/pedrohenrique505/downhill-revived/issues/12).

Implementation continuation: [finding 076](076-linux-vulkan-exact.md). The
results below describe the original missing-backend build.

## Results

SDL3 created a Wayland window with its OpenGL renderer on this host. The attached Xbox 360
controller was detected as player 1. Startup movies were skipped. The game used the player's USA
ISO, referenced in place. Tests ran from `build/port/run` against `../link/dhd`.

| Requested check | Result |
| --- | --- |
| Picture | FAIL: internal captures at fields 120 and 600 are entirely black. |
| Windowed speed | Field 120 at 2.032343 s; field 600 at 10.176815 s (about 59 fields/s from startup). No rasterization: this is not renderer throughput. |
| Keyboard/controller in menus and riding | Blocked by missing picture. Controller detection alone does not validate input. |
| Ctrl+C | SIGINT delivered to the test process terminates it in about 64 ms, return code -2 in Python. A physical terminal keypress was not tested. |
| Close button | Unverified. Automated Hyprland close attempts did not terminate the process; the harness's targeting was not independently validated, so this is not yet a confirmed game bug. Each process was explicitly killed afterward. |
| Same-screen PCSX2 comparison | Blocked: no native title/menu/race picture and no installed PCSX2 executable available in PATH. |

The issue remains open for visual, input and renderer-performance acceptance after #12.
No renderer implementation is added by this validation task.

## Reproduction and objective failure signal

Run with the player's ISO path in `DHD_ISO`:

```sh
mkdir -p /tmp/dhd-issue1-before
cd build/port/run
DHD_RENDER=exact DHD_SKIP_MOVIES=1 \
DHD_GS_CAPTURE=/tmp/dhd-issue1-before \
DHD_GS_CAPTURE_FRAME=600 DHD_GS_CAPTURE_FIELDS=1 ../link/dhd
```

Stop the test process after the capture. Inspect the BMP without image-library dependencies:

```python
import struct
from pathlib import Path
b = Path('/tmp/dhd-issue1-before/frame-000-native-000.bmp').read_bytes()
offset = struct.unpack_from('<I', b, 10)[0]
width, height = struct.unpack_from('<ii', b, 18)
assert struct.unpack_from('<H', b, 28)[0] == 32
nonblack = sum(any(b[i:i+3]) for i in range(offset, len(b), 4))
print(width, abs(height), nonblack)
assert nonblack > 0, 'black GS scanout'
```

Observed at field 600: `640 448 0`, followed by `AssertionError: black GS scanout`.
At field 120: 512 by 448, also all RGB zero. This is a boot smoke signal, not a replacement for
pixel-exact renderer acceptance. Captures and debugger logs remain in `/tmp/dhd-issue1-before/`;
no game data or captures are committed. A PNG copy of field 600 is `field-600.png` there.

## Diagnosis

Ranked hypotheses were incorrect scanout selection, missing draw execution, and SDL presentation.
The internal BMP being black excludes a presentation-only failure. The terminal prints only
circuit 1's DISPFB/DISPLAY; these zeros are misleading because the game enables circuit 2.
GDB at field 601 reports:

```text
vsync=601
draw callback=(nil)
PMODE=0x8066
SMODE2=3
DISPFB1=0, DISPLAY1=0
DISPFB2=0x948c, DISPLAY2=0x001bf9ff0183227c
```

The GS freeze contains nonzero uploaded VRAM. `gs_reset()` clears all callbacks;
`vertex_kick()` only rasterizes when `g->draw` is installed. Windows installs it in
`exact_init()` through `gs_gpu_init()`. Linux's `link.py` excludes `gs/gpu.c`, and its SDL presenter
never installs a replacement. Thus commands and VSync advance, but primitives never draw.
`gs_display()` only decodes already-written VRAM. The existing exact rasterizer is a D3D11
compute shader (`raster.hlsl`), not a CPU implementation in `gs.c`. The inaccurate presenter
comment has been corrected. No runtime workaround or weakening of matching checks is made.

A real fix requires a Linux raster backend with texture/depth/blending and coherent transfers
and readbacks, followed by the outstanding acceptance checks. Changing the ISO, skipping more
movies, or treating 60 VSync/s as working rendering does not implement that backend.

## Validation and sources

Existing GS suite built with:

```sh
clang -m32 -O2 -msse2 -ffp-contract=off port/gs/test_gs.c port/gs/gs.c \
  port/gs/memory.c port/gs/display.c -o /tmp/dhd-test-gs
/tmp/dhd-test-gs
```

Result: `upload fuzz: 3000 transfers, 6657394 pixels, 0 differing` and
`GS memory, GIF, transfer and CLUT tests: 0 failures`. These tests exercise core storage and
scanout; they do not establish a functioning Linux rasterizer. No new tests or test seams were
introduced. Only documentation/comments change, so game rebuild and Windows-only full suites
are not relevant verification of this diff.

Online references consulted before considering a solution:
- [SDL_CreateWindow](https://wiki.libsdl.org/SDL3/SDL_CreateWindow),
  [SDL_RenderPresent](https://wiki.libsdl.org/SDL3/SDL_RenderPresent), and
  [SDL_IsMainThread](https://wiki.libsdl.org/SDL3/SDL_IsMainThread): SDL thread requirements.
- [Hyprland dispatchers](https://wiki.hypr.land/Configuring/Basics/Dispatchers/) and
  [Lua snippets](https://wiki.hypr.land/configuring/code-snippets/): attempted process-specific
  close requests; this automation remains unverified.

The missing-backend diagnosis comes from this repository and the live callback/register checks,
not from an SDL thread hypothesis. No external source code was copied.
