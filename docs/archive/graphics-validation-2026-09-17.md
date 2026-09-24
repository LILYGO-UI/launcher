> 历史记录：本文保留当次优化、验证或部署的上下文，不代表当前代码或设备状态。
> 当前设计见 [图形渲染设计](../graphics-rendering.md)；[返回归档索引](README.md)。

# CM0 Renderer Comparison and Application Integration

This report records the September 17 renderer comparison and its deployment.
The subsequent [switch latency work](app-switch-latency-2026-09-17.md) adds
context retention and a private VC4 runtime. See the [hardware test guide](../../tests/README.md)
for current reproduction commands; these measurements do not validate later
code changes.

## Decision

Keep **DRM/KMS + EGL/GBM + the OpenGLES texture-cache draw unit** for the
production Launcher and applications. NanoVG GLES2 is now an opt-in AppKit
renderer (`LILYGO_UI_RENDERER=nanovg`) with working display recreation and
rotation, but remains experimental because its shadow and arc output differs
visibly from the software/OpenGLES reference.

The SDK default remains `software`, preserving compatibility with the pinned
BSP. Device GPU builds use the target-matched graphics sysroot. Applications
consume an installed standalone SDK with `find_package(LilyGoUI CONFIG
REQUIRED)`; their sources are not included in the Launcher build.

## Target and Method

- Raspberry Pi Compute Module 0 Rev 1.0, AArch64, kernel 6.18.34+rpt-rpi-v8.
- VC4 V3D 2.1, OpenGL ES 2.0, Mesa 26.2.0-1~bpo13+0~rpt3.
- DSI connector 44, 568x1232 at 35 Hz; Linux reports 415 MiB total RAM.
- LVGL 9.5.0, Release cross builds. NanoVG successfully selected a window
  configuration with stencil-8 and 4x MSAA.
- Three backends, two scenes, two orientations: 12 runs and 36 display
  create/delete cycles. Each cycle has one second warmup and eight measured
  seconds. The list scene reuses text/icons while scrolling; the dynamic
  scene continuously changes arcs, labels, bars, colors, radii and shadows.
- Captures use the same animation time, 1000 ms, after measurement. CPU is
  relative to one fully occupied core. Aggregates are weighted by elapsed time
  or frame count.

## Results

| Scene | Orientation | Renderer | CPU | LVGL Refreshes/s | Mean Refresh | Peak RSS |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| List | Portrait | Software | 56.41% | 30.24 | 21.90 ms | 19.41 MiB |
| List | Portrait | OpenGLES | 36.49% | 30.23 | 11.77 ms | 79.55 MiB |
| List | Portrait | NanoVG | 28.83% | 30.20 | 9.12 ms | 80.40 MiB |
| List | Landscape | Software | 70.51% | 26.91 | 32.28 ms | 21.16 MiB |
| List | Landscape | OpenGLES | 22.21% | 30.24 | 7.04 ms | 79.92 MiB |
| List | Landscape | NanoVG | 15.85% | 30.24 | 4.73 ms | 83.14 MiB |
| Dynamic | Portrait | Software | 91.24% | 11.03 | 88.95 ms | 21.16 MiB |
| Dynamic | Portrait | OpenGLES | 72.25% | 30.20 | 23.17 ms | 79.69 MiB |
| Dynamic | Portrait | NanoVG | 21.76% | 30.24 | 5.70 ms | 82.96 MiB |
| Dynamic | Landscape | Software | 57.34% | 17.50 | 55.41 ms | 21.16 MiB |
| Dynamic | Landscape | OpenGLES | 66.85% | 30.23 | 21.33 ms | 80.26 MiB |
| Dynamic | Landscape | NanoVG | 21.82% | 30.23 | 5.72 ms | 82.80 MiB |

NanoVG uses about 70% less CPU than OpenGLES in the dynamic portrait scene,
with about 3.3 MiB additional peak RSS. However, its visual output is not
equivalent: the dynamic captures show broad gray shadow bands and stray
colored/gray fragments near arc rings in both orientations. Software and
OpenGLES match visually for this fixture. The list captures show correct
layout, text, icons, clipping, rotation and transparent overlay in all three
backends. The NanoVG numbers indicate optimization potential, not an accepted
improvement at equivalent image quality.

Refresh throughput counts LVGL refresh completion, not completed GPU work or
physical scanout FPS. No input-to-photon measurement was performed. The panel
mode remains 35 Hz. Peak RSS covers each entire process, including startup,
three display cycles and capture; it is not complete GPU memory accounting
and is not directly comparable to the shorter September 8 memory sample.

## Fixes and Verification

- NanoVG releases glyph/image/FBO resources while the old EGL context is
  current, retains one registered draw unit, and recreates resources on return.
- Root drawing and clipping rotate into native window coordinates; child
  layers retain logical coordinates. NanoVG no longer allocates an unused
  full-screen CPU framebuffer and spare texture for DRM presentation.
- FreeType glyph acquisition uses a local descriptor, keeping the NanoVG
  red-black-tree key immutable. This fixes the on-board cache-eviction crash.
  Returned glyph buffers and font-cache references are handled explicitly.
- Cache-pressure flushes retain active-frame state and transforms, so the
  final commands of a task are submitted after an eviction.
- Evdev delivers each complete pointer `SYN_REPORT` sample. Previously,
  a queued press and release could collapse into a release when a cold frame
  delayed polling; this caused a missed second launch in the hardware test.
- SDK: seven host CTests passed, including EGL/NanoVG lifecycle, glyph cache,
  cache pressure and evdev (gestures enabled and disabled). Launcher: seven
  host CTests passed. Renderer switching across software/OpenGLES/NanoVG and
  repeat patch application passed.
- The final NanoVG Launcher completed eight temporary application launches
  and returns plus landscape startup/shutdown, using 150 ms FIFO taps.
- Settings, Files, Meshtastic and Template independently cross-compiled and
  packaged with GLES dependencies, no RPATH, and the required font notices.
  The installed SDK now retains font licenses for consumer packaging.
- The production GLES candidates completed Settings/Files/Meshtastic launches
  and clean returns twice in each orientation: 12 real application handoffs.
  The NanoVG Launcher completed six handoffs with those same GLES apps.
  Input was restricted to temporary Launcher icons; application controls were
  not changed. XDG state was isolated, Files used an empty temporary directory,
  and Meshtastic used a loopback endpoint rather than a radio connection.

NanoVG remains experimental until its shadow/arc fidelity and more complex
off-screen layer behavior are resolved. Its off-screen FBOs are single-sample;
they do not inherit window MSAA. Gradients are limited to two colors.

## Artifacts and Reproduction

- Raw data, hashes, resource samples, logs and 12 captures:
  `build/graphics-results-20260917/comparison/`.
- Launcher packages: `build/graphics-packages-20260917/{opengles,nanovg}/`.
- Independent application packages: `build/apps-opengles/packages/`.
- Package/ELF validation: `build/graphics-packages-20260917/validation.json`.
- Target test artifacts: `/tmp/lilygo-ui-nanovg-validation/`.

Use the SDK's [comparison runner](https://github.com/LILYGO-UI/cm0-appkit/blob/main/docs/graphics-benchmark.md)
with exclusive DRM ownership already arranged. It records binary hashes,
separate stdout/stderr, CPU/RSS, per-cycle timing and bounded command exits.
The hardware scripts `tests/test_drm_runtime.py` and
`tests/test_app_drm_runtime.py` exercise temporary and real application handoff.

No video hardware codec path was added; it is independent of UI rendering.

## Deployment and Recovery

Installed the validated OpenGLES packages on `pi@192.168.8.206` for
`lilygo-ui-launcher`, `lilygo-ui-settings`, `lilygo-ui-files`, and
`lilygo-ui-meshtastic` (all retain version 0.1.0). Template 0.1.1 was built
and packaged only. The target's AppKit 1.2.0 and shared fonts remain in place;
the local development SDK package was not installed over them.
The installed binaries matched the validated package hashes. Each installed
application then passed another portrait and landscape launch/clean-return
cycle (six further handoffs). The Launcher service was restarted afterwards.

Before installation, the 32 existing package-owned files were archived at
`/var/lib/lilygo-ui-launcher/graphics-backup-20260917/installed-files.tar.gz`
on the target. This includes the old executables and package assets/defaults,
with an inventory and executable hashes alongside it. To restore those files:

```sh
sudo systemctl stop lilygo-ui-launcher
sudo tar -xzf /var/lib/lilygo-ui-launcher/graphics-backup-20260917/installed-files.tar.gz -C /
sudo systemctl start lilygo-ui-launcher
```

This restores the prior package-owned files, not Debian dependency metadata;
the installed versions remain 0.1.0. It does not restore per-user application
data. Installing a newer package afterwards replaces the restored files.
