> 历史记录：本文保留当次优化、验证或部署的上下文，不代表当前代码或设备状态。
> 当前设计见 [图形渲染设计](../graphics-rendering.md)；[返回归档索引](README.md)。

# CM0 Graphics Validation

Validated on 2026-09-08 from the development host. The target clock was two
days behind; target log timestamps are not the report date.

## Hardware and Build

- Raspberry Pi Compute Module 0 Rev 1.0, AArch64 Linux 6.18.34+rpt-rpi-v8.
- Kernel display driver: `vc4-drm`; EGL/GLES renderer: Broadcom `VC4 V3D 2.1`.
- OpenGL ES 2.0, EGL 1.5, Mesa `26.2.0-1~bpo13+0~rpt3`.
- Connected DSI-1: connector 44, native 568x1232 at 35 Hz.
- Active primary plane 76 exposes rotation 0/180 and reflections, but no
  90-degree rotation. Landscape therefore uses GPU texture rotation.
- LVGL 9.5.0, GCC 15.2.0, Release builds of the same AppKit benchmark.
- The original BSP 0.1.0 cache was preserved. A separate copy of its sysroot
  was supplemented with matching target GBM/EGL/GLES development headers,
  pkg-config files and libraries for the GPU cross build.

## Performance

`lilygo-ui-graphics-benchmark` rendered the same scrolling application list,
fonts and moving translucent overlay. Each combination ran three display
create/delete cycles, with one second of warmup followed by eight measured
seconds per cycle. CPU percentage is relative to one fully occupied core.
The following aggregates are weighted by measured elapsed time or frame count.

| Orientation | Renderer | CPU | LVGL Refreshes/s | Mean Refresh | Per-Cycle P95 Range |
| --- | --- | ---: | ---: | ---: | ---: |
| Portrait | Software | 57.89% | 30.15 | 22.77 ms | 32.41-32.88 ms |
| Portrait | GLES2 | 40.02% | 30.26 | 12.91 ms | 15.10-15.48 ms |
| Landscape | Software | 69.53% | 26.20 | 33.69 ms | 47.66-47.73 ms |
| Landscape | GLES2 | 26.89% | 30.24 | 8.53 ms | 10.23-10.59 ms |

CPU use decreased by about 31% in portrait and 61% in landscape. These are
benchmark results, not measured launcher interaction FPS. Refresh throughput
counts LVGL refresh completion, not physical scanout completion. The panel
mode and LVGL refresh timer also limit visible throughput.

A separate three-second landscape run measured process peak RSS using
`resource.getrusage(RUSAGE_CHILDREN)` after running the benchmark: software
10,724 KiB (10.47 MiB), GLES2 76,840 KiB (75.04 MiB). This includes driver
and process allocations but is not a complete GPU memory accounting. GPU
rendering trades additional memory for lower CPU work.

## Validation

- Launcher: all seven host CTest tests passed.
- AppKit: system status, typography and new EGL lifecycle tests passed.
- Software and GPU device builds and Debian packaging passed.
- Configuration switching software -> GLES2 -> software -> GLES2 passed.
- The unmodified BSP rejects GPU configuration with a specific missing
  GBM/EGL/GLES dependency message.
- All twelve benchmark cycles completed; portrait and landscape composed
  frame captures were nonblank, with correct fonts, colors and transparency.
- The actual GPU Launcher started, launched the temporary `/usr/bin/true`
  application and returned three times, then started and stopped in landscape.
  The hardware smoke test uses a FIFO, without injecting into real input devices.
- GLSL ES 1.00 is selected directly for the EGL GLES2 context; the final build
  no longer emits failed GLSL 3.00/3.30 probe warnings during normal startup.

Captures read the composed logical texture after measurement. They do not
independently verify physical panel rotation or input-to-photon latency.
The mock lifecycle test checks cleanup ordering and software draw selection;
the hardware tests additionally exercise actual EGL/DRM resource recreation.

## Deployment

Installed the validated `lilygo-ui-launcher_0.1.0_arm64.deb` on
`pi@192.168.8.206`. The installed executable matches the tested package
(SHA-256 `e1c5342622a4aa0449f71c223816d669564d6fb1bbdd84fe1cc6039a53e0d70f`).
The service is active with zero automatic restarts after installation; its
startup log reports `VC4 V3D 2.1`, `renderer=opengles`, and the first UI frame
ready at 568x1232. A ten-second idle sample measured approximately 1.2% of one
CPU core and 79.74 MiB RSS. This short idle sample is separate from the animated
benchmark above and does not establish long-term stability.

The target's installed AppKit development package remains at 1.2.0. Other
applications retain their existing builds. Development and diagnostic tools
(`libgbm-dev`, `libegl-dev`, `libgles-dev`, `mesa-utils`, and `libdrm-tests`,
with their dependencies) were installed for validation.

The original launcher executable is preserved on the target. To restore its
software rendering behavior while retaining the installed package metadata:

```sh
sudo systemctl stop lilygo-ui-launcher
sudo cp --preserve=all /var/lib/lilygo-ui-launcher/lilygo-ui-launcher.pre-gpu-20260908 /usr/bin/lilygo-ui-launcher
sudo systemctl start lilygo-ui-launcher
```

This restores the executable only. Reinstalling the GPU package replaces it
again. Final smoke, deployment, idle sampling, and memory logs are retained
alongside the benchmark results in `build/graphics-results/`.

## Usage

Select `-DLILYGO_UI_RENDERER=opengles` when building Launcher or an independent
application through `find_package(LilyGoUI CONFIG REQUIRED)`. The SDK default
remains `software` for compatibility with the pinned BSP and other products.
Selection is at build time; failed EGL initialization does not automatically
switch to another DRM renderer. Existing statically linked apps need rebuilding.

See [AppKit graphics configuration](https://github.com/LILYGO-UI/cm0-appkit/blob/main/README.md#device-graphics),
[diagnostics](https://github.com/LILYGO-UI/cm0-appkit/blob/main/docs/graphics-diagnostics.md), and
[benchmark instructions](https://github.com/LILYGO-UI/cm0-appkit/blob/main/docs/graphics-benchmark.md).

On the validation host, raw JSON, logs and captures are retained under
`build/graphics-results/`. The generated Launcher package is under
`build/graphics-packages/`. Target-side diagnostics and test artifacts are in
`/tmp/lilygo-ui-graphics-validation/`.
