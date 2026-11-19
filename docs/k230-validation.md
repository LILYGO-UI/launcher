# K230 adaptation validation — 2026-11-07

## Target and build

Validated on a physical T-Display-K230, reporting Linux 6.6.36, riscv64,
Buildroot 2025.02.1, and `Canaan CanMV-K230 with RM69A10 OLED`. The board uses
glibc 2.33, a 568×1232 DSI panel, and `goodix_ts` at `/dev/input/event1` with
X=0..1060 and Y=0..2400.

AppKit, Launcher, Template, and the existing Calculator were built as separate
top-level CMake projects. Calculator required **no source changes**. All apps
used the same AppKit source through `find_package(LilyGoUI CONFIG REQUIRED)`.
The K230 profile selected software rendering, RGB565, and the static SDK.

The deployment test used an ARM64-hosted Ruyi/Xuantie GCC 14.1.1 toolchain with
glibc 2.33 development files, the exact v0.2.9 image runtime libraries, and
OpenSSL 3.4.1 headers. The repository toolchain wrapper was exercised with a
Buildroot toolchain file generated from the official 2025.02.1 template. This
was an ABI verification sysroot, not a complete rebuild of the official image;
production builds should use that image's completed Buildroot `host/staging`.
The compiler and runtime hashes, header provenance and caveats are retained in
`build/k230-validation/k230-validation-provenance.json`.

A C++17 thread probe executed successfully on the board. The target ELF loader
resolved application and transport-helper dependencies against board libraries.
The final Launcher preflight passed all 11 checks. No Debian glibc libraries
were installed on the target.

## Device results

- Launcher renders and discovers independently installed manifests.
- Goodix capabilities and calibration are read from evdev; no fixed event
  number is compiled into the K230 profile.
- Bounded test taps/swipes injected through the real Goodix evdev node launched
  applications, changed Template's counter, and returned using the bottom home
  gesture in portrait and landscape. This validates the input pipeline and
  coordinate mapping; it is not a claim of a manual capacitive-touch test.
- Existing Calculator displayed `7 + 8 = 15` and returned to Launcher with exit
  code 0. Its measured first frame was 143 ms; Launcher restoration was 172 ms.
- Final landscape Template first frame was 110 ms and restoration was 189 ms.
- Actual scanout buffers were captured using read-only DRM queries/mmap. The
  native frame is RGB565, 568×1232, pitch 1136. The final plane rotation property
  was `1` (`rotate-0`) in both orientations and across app handoff.
- An inherited plane rotation of `8` (`rotate-270`) from the stock shell was
  found during validation. The new atomic plane-reset patch corrects this;
  raw framebuffer screenshots alone would not have detected the extra rotation.
- Standard `rm69a10` backlight sysfs is present. Battery `power_supply` telemetry
  is absent in this BSP; the UI correctly shows `--`, distinct from measured 0%.
- BusyBox 1.37 service start/status/repeated-start/stop/repeated-stop/restart and
  stdout/stderr logging were tested with a harmless target-compiled daemon in
  an isolated directory. All tests passed and all test processes were removed.

The graphics benchmark completed three portrait and two landscape display
creation/deletion cycles, each with a 1-second warmup and 2-second measurement:

| Scene | Refresh rate | CPU usage, one-core basis | Result |
| --- | --- | --- | --- |
| Scrolling list, portrait | 52.187–52.195 FPS | 23.492–23.496% | 3/3 cycles passed |
| Scrolling list, landscape | 52.182–52.187 FPS | 63.546–63.715% | 2/2 cycles passed |

Both benchmark processes exited 0 with empty stderr and produced RGB565 PPM
captures. These short measurements describe this scene and build, not a
performance guarantee for every application.

## Host and package checks

- AppKit: 29/29 CTests for CM0 and 29/29 for the K230 simulator.
- Launcher: 31/31 final host tests excluding transport/Bluetooth; the transport
  test passed separately with loopback permissions. Bluetooth integration was
  skipped on the host without a suitable bus.
- Template: 4/4 tests including portrait and landscape rendering.
- Driver tests cover independent scanout staging, padded strides, RGB565 and
  XRGB8888, rotation, stale hardware plane rotation, optional property absence,
  atomic failure/retry, allocation lifetime and repeated configuration.
- DRM and fbdev-only Linux branches compiled; fbdev-only output had no DRM
  linkage. Full RISC-V device builds and real CPack TGZ packaging succeeded.
- The archive checker verifies RISC-V ELF64 application identity, one primary
  executable per app, shared font ownership, licenses, source SDK/toolchain
  files, SysV service layout, and no leaked LVGL static libraries/headers.

Run the archive check against the real delivered files:

```sh
python3 /path/to/appkit/tests/check_k230_archives.py \
  dist/k230/lilygo-ui-appkit-dev-0.1.1-all.tar.gz \
  dist/k230/lilygo-ui-launcher-0.1.3-k230-riscv64.tar.gz \
  dist/k230/lilygo-ui-template-0.1.2-k230-riscv64.tar.gz \
  dist/k230/lilygo-ui-calculator-k230-riscv64.tar.gz
```

Packages and SHA-256 checksums are in `dist/k230/`. Logs, provenance and actual
DRM screenshots are in `build/k230-validation/`; these generated artifacts are
Git-ignored. The board test installation is under
`/tmp/lilygo-ui-k230-20261107`, with common fonts installed once. The original
K230 service and its boot configuration were preserved and restored after
testing. No reboot or system package replacement was needed.

Camera/media, cellular and other hardware-specific app backends are outside
this shared UI adaptation. The stock root kiosk also retains the launcher's
existing non-root requirement for file sharing.

## Subsequent system deployment

Permanent system installation was also validated on November 7, 2026. The
launcher package now includes `S45lilygo-ui-network`, which migrates the stock
shell's Wi-Fi profile into root-only persistent configuration and owns WPA and
renewing DHCP independently of the graphical UI. Migration equivalence was
checked on the target without printing or exporting credentials.

A complete reboot reached the new UI and wireless SSH after 23 seconds. Both
WPA and DHCP were tracked by the independent service, all 97 payload files and
modes matched the final archives, and Calculator launch/return succeeded after
reboot. The BSP's NTP service subsequently synchronized the clock. Original UI
boot files remain backed up outside the `S??*` startup pattern.

The network helper has 27 regression tests covering credential migration,
manager preservation, cancellation, PID reuse, and delayed daemon PID files.
The latter covers the target's daemonizing-parent timing: startup waits for a
live daemon identity before advancing from authentication to DHCP.

The current deployment receipt and restoration command are recorded in
`build/k230-validation/DEPLOYMENT.md`; the earlier temporary test state described
above preceded this permanent installation.
