# 078: Linux VU acceleration and overlapping compute submissions

2026-10-24, fork issue #1, following finding 077. This step improves performance;
real-time play and the issue's independent-renderer acceptance remain open.

## Feedback loop and profile

The existing actual-race GS replay still fails its 30-fields/s raster budget:

```
python port/tools/bench_exact_linux.py /tmp/dhd-issue1-vulkan-accept/frame-009.gs --max-ms 33.333
Replay: fields=3 raster_ms=130.018 ms_per_field=43.339
FAIL: exact raster exceeds 33.333 ms/field
```

That three-field active replay remains the small, unattended performance seam;
it exercises real GIF/GS work, not a frozen framebuffer. The original menu
counter/field window is also repeated in GDB for full-game measurement. The
new `port/tools/bench_window_linux.py` makes that read-only measurement reusable,
adds a performance assertion and bounds the owned process lifetime. It requires
`DHD_ISO`, navigates using the existing public pad script, and records fields,
menu increments, buffer changes and race ticks separately. Boot makes this
full-game validation slower than the isolated replay; it is not the tight loop.

Ranked predictions were CPU scanout/upload, forced GPU read synchronization,
shader raster cost, and VU work limiting the full game. Existing opt-in worker
and frame profiles plus temporary presenter timers distinguish them. Baseline
20..30 wall seconds (`/tmp/dhd-r78-{frame,vu,gs}.csv`):

- Main thread: 9.972229 s, 92 menu frames; 9.486105 s between VSync entries,
  0.093942 s first graphics wait, 0.369354 s presentation.
- VIF/VU worker: 10.003479 s, 92 frames, 8.780498 s busy, 8.998748 s CPU.
- GS worker: 9.999956 s, 92 frames, 4.908989 s busy, 2.120864 s CPU.

These are overlapping thread intervals, not additive frame costs. A separate
420..480-field presenter sample spent 0.118043 s scanout (including sync),
0.040951 s texture upload and 0.012890 s pacing/presentation. A GPU-only scanout
would offer little improvement to this menu, so that hypothesis was rejected
as this step's priority. No scanout backend or image behavior was changed.
Temporary `[DEBUG-r78]` timers stayed in `/tmp` source copies; the final build
uses the original presenter and ordinary kernel object.

## Confirmed causes and implementation

The Linux `vu0_interp.c.o` was only 63 KiB and contained no translated blocks.
`link.py` enabled `PS2_VU_AOT` only with `DHD_BUILD_VU_AOT=1`, whereas the Windows
release wrapper always supplies it. Ordinary Linux builds silently lost the
existing translated VU1 and guarded native VU0/VU1 implementations.

Linux now defaults to AOT in the ordinary runtime build, with an explicit
`DHD_BUILD_VU_AOT=0` retaining interpreter-only builds. Windows' ordinary-link
policy is unchanged; its release wrapper still enables AOT. The generated
microcode remains under ignored `build/`, with runtime signatures loaded from
the player's own data. Runtime unknown/modified-code fallbacks are unchanged.
`DHD_VU_INTERPRETER=1` still selects diagnostic interpreter behavior.

Before the fix, `python port/tools/test_link_vu.py` failed the default-Linux
case (`'-DPS2_VU_AOT'` absent from the actual runtime compile command). After:
all three cases pass (default, explicit 0, explicit 1). The test also checks
that only the VU source receives the AOT compilation define.

The remaining Vulkan capacity waits were changed independently: two banks,
each with its own command pool, command buffer, fence and 16 immutable input
slices. Capacity submits return without waiting; preparing the other bank can
overlap GPU execution. A bank retires before its input slices are rewritten.
Host VRAM reads/uploads and deferred-page snapshots drain both banks. All
commands stay on one queue, retaining compute/transfer barriers and FIFO
texture-snapshot ordering. This adds approximately 65 MiB beyond finding 077's
input allocation; no VRAM/image formats or shader arithmetic change.

## Independent VU verification

`port/tools/test_vu1_pcsx2.py --aot` now also builds natively on i686 Linux
instead of requiring Windows clang-cl libraries. It retains the original
pinned PCSX2 hashes, adapters, fixtures and full-state comparisons:

- 1,584 seeded cases over all 32 VU1 entries: zero differences against PCSX2
  and the separately linked original interpreter.
- Synthetic VU instruction, timing, continuation and XGKICK tests: zero failures.
- Live captures from the final run are also checked with that independent
  oracle; results are in `/tmp/dhd-r78-captured-vu-oracle.json`.
- The existing `vu0_native_diff.c` linked with oracle native/reference objects
  checks five locally captured VU0 native entries, one million raw-bit cases
  per entry. All five million full CPU/memory comparisons MATCH, all 304
  changed-instruction guards decline acceleration, and invalid VI0 guards pass.
  Expected unknown-instruction diagnostics occur only in changed-word controls.

No new VU arithmetic implementation was introduced; this restores the existing
production paths and verifies them on the Linux host.

## Measurements and GS verification

With no other GPU test running, sequential real-menu fields 420..480:

| Version | Wall seconds for 60 fields/menu increments | Actual menu frames/s |
| --- | ---: | ---: |
| Finding 077 executable | 6.764169 | 8.870 |
| AOT plus two-bank Vulkan | 3.060132 | 19.607 |

Logs: `/tmp/dhd-r78-pair-base.log`, `/tmp/dhd-r78-pair-after.log`. An AOT-only
exploratory sample reached 16.574/s; it is not substituted for the final paired
measurement. These remain monitored GDB samples on this host, not a 60 FPS claim.

Five interleaved isolated baseline/async pairs, keeping VU outside the replay:

| Capture | Baseline median ms/field | Async median ms/field |
| --- | ---: | ---: |
| Menu frame-001 | 31.895 | 25.929 |
| Race frame-009 | 42.319 | 36.206 |

All eleven original captures retain byte-identical full VRAM. Replay now also
writes `.readbacks` responses and optionally compares them, but these eleven
dumps contain no read records: their empty-file matches are **not** evidence
of nonempty readback equivalence. The existing GPU fixtures supply nonempty
readback/deferred-read coverage and pass, including packed formats, aliases,
feedback, transfers and the 48-state slot-reuse test. Evidence:
`/tmp/dhd-r78-async-suite/{results,repeated}.json`.

## Sources and remaining acceptance

Consulted [Khronos command-buffer lifecycle](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html)
and [synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
for pending-buffer ownership, fences, queue ordering and host visibility.
[SDL texture wrapping](https://wiki.libsdl.org/SDL3/SDL_CreateTextureWithProperties)
was researched for the scanout hypothesis, then deferred after profiling.
Independent VU semantics come from the existing comparator's pinned
[PCSX2 interpreter](https://github.com/PCSX2/pcsx2/tree/0f71e668b8adc950db7de1b1b15c2f5080c102e9/pcsx2),
downloaded with the existing SHA-256 checks into ignored build storage. No
external implementation was copied into production code.

Heavy race replay still exceeds the 33.333 ms raster budget. Independent retail/
D3D11 renderer parity and the two race pixel outliers from finding 076 remain
open. Matching source and strict checks are unchanged; no fresh retail ELF
MATCH is claimed. Final live-race and clean diagnostic checks are recorded below.

## Final window and regression results

The new unattended benchmark was executed against both actual executables:

```
DHD_ISO=<local ISO> python port/tools/bench_window_linux.py --exe /tmp/dhd-r78-base --out /tmp/dhd-r78-window-before --min-fields 15
fields=60, menu_frames=60, buffer_changes=60, fields_per_s=8.777064
FAIL: below 15.0 fields/s

DHD_ISO=<local ISO> python port/tools/bench_window_linux.py --out /tmp/dhd-r78-window-after --min-fields 15
fields=60, menu_frames=60, buffer_changes=60, fields_per_s=19.604544
exit 0
```

This threshold checks the improvement being delivered; it does not replace or
relax the still-failing 30-fields/s race raster budget above. Both runs are
windowed, sequential, and have the same navigation and game-counter increments.

Final windowed race, fields 3500..3561: phase 2, 61 race-clock ticks in
3.046384 s (20.024 fields/s). This is a field/tick measurement, not an independent
unique racing-frame count. `/tmp/dhd-r78-race-live.log` records the bounded run;
`/tmp/dhd-r78-race-captures/frame-000-native-002.bmp` shows the riders at the
starting gate at field 3002. It is a smoke capture, not retail image parity.
All 56 VU1 inputs collected through the completed run also MATCH the independent
PCSX2/original-interpreter comparator (1,640 seeded/live VU1 cases total).

The ordinary, non-profiled executable is refreshed at `build/port/link/dhd` and
`build/port/run/dhd`. No game data, captures, microcode or executable is tracked.
No issue comment, push or release was published.

The post-change diagnostic profile (10..18 wall seconds in the same menu)
records 168 frames: VIF/VU busy 2.548232 s (~15.17 ms/frame), GS busy
6.097548 s (~36.29 ms/frame), total frame interval 7.989749 s (~47.56
ms/frame). The thread intervals overlap and are not summed. The bottleneck
has moved toward GS execution/waits; these are not pure shader timings.
Profiles: `/tmp/dhd-r78-{frame,vu,gs}-after.csv`. The additional diagnostic
executable stays in `/tmp`; ordinary final runtime objects were restored and
relinked before handoff.

Final async negative control deliberately writes every dispatch's constants and
primitives into slot zero: the fixtures reject it (21,973 failed checks, exit
1). The correct backend immediately passes the same suite. Replaying the new
three-field gate capture through the original and async backends also retains
byte-identical full VRAM. Its read-response file is empty too, so it adds no
nonempty-readback coverage. Evidence: `/tmp/dhd-r78-negative.log` and
`/tmp/dhd-r78-fresh-{base,after}.vram`. Python compilation, build-policy/block
regressions and `git diff --check` pass; no temporary debug markers remain in
production sources or the final executable.
