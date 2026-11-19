# LILYGO UI Launcher

For T-Display-K230 Linux builds and the BusyBox/SysV service, see
[K230 adaptation](docs/k230.md).

English | [简体中文](README.zh-CN.md)

LILYGO UI Launcher is an LVGL application launcher for LILYGO Linux products.
CM0 is the first supported product profile. The current device
build uses Linux DRM/KMS and evdev directly, with fbdev retained as a fallback;
the host build uses an SDL2 window. Installed applications are independent executables discovered from JSON
manifests, so installing or removing a compatible `.deb` changes the home
screen without rebuilding the launcher.

The device and simulator default to the product UI's 568x1232 portrait canvas.
The home screen uses a 96 px AppKit status bar and shows up to 18 applications per
page in a 3x6 portrait grid or 6x3 landscape grid. Press Space or swipe down from
the status bar to open Quick controls and switch orientation; press Space again
or Esc to close the panel. Swipe horizontally or use the page indicators to reach
additional applications. On the host, the simulator automatically scales its
window to fit the current display while
preserving that logical canvas. The SDL host window uses high-DPI rendering so
the scaled interface remains crisp.
`--width` and `--height` set the unrotated portrait base canvas for diagnostics;
use `--orientation portrait|landscape` to select its initial direction. The SDL
window remains freely resizable for testing smaller and wider viewports.

On the device, connecting a TCA8418 keyboard selects landscape, including when
it is already connected at startup. Disconnecting it keeps the current
orientation. Quick controls can always switch either way; that choice is kept
while the keyboard stays connected, including after returning from an app.
Connecting the keyboard again selects landscape. Other keyboards and the SDL
simulator's host keyboard do not trigger automatic rotation.
After signing in on the device, Quick controls also provides **Lock screen**.
Locking keeps the current account session and requires that account's password
to unlock. **Sign out** ends the session instead. Quick controls also provides
iOS-style **Power off**, **Brightness**, and **Volume** cards. Brightness and
volume can be dragged vertically or adjusted from a keyboard in five-percent
steps. Power off always asks for confirmation; the simulator shows a safe
completion message instead of powering off the host.
Connection detection reads the TCA8418 configuration register through
`/dev/i2c-*`. If unplugging resets the chip while its input node remains,
Launcher rebinds that TCA8418 client to the kernel driver to restore matrix
scanning and interrupts, then discovers the replacement input device. Failed
recovery is retried without overriding a manual orientation choice. This needs
access to the keyboard's I2C adapter and the driver's sysfs bind/unbind controls;
the installed root service has those permissions.
Recovery continues while Launcher waits for a foreground application. Apps
use AppKit's keyboard deletion and reconnection handling to open the new evdev
node; because AppKit is statically linked, existing applications must be rebuilt
with that SDK change to recover without restarting the application.

## Project structure

```text
src/
  *.cpp, *.h  Launcher runtime and UI sources
tests/        Test and UI-rendering sources
docs/         Design documents and archived validation/deployment evidence
assets/
  images/     Image assets
  audio/      Audio assets
  fonts/      Font assets
  app-icon.png  128x128 release icon
  previews/     Release preview images (not installed)
```

Launcher sources live directly under `src/` and launcher tests under `tests/`.
Applications live in independent repositories and are not stored, compiled,
tested, or packaged by the launcher repository.

See the [design documentation index](docs/README.md) for lifecycle, graphics,
SDK packaging, and UI design requirements. Dated optimization and deployment
reports are preserved in the [historical archive](docs/archive/README.md).

LILYGO UI AppKit is maintained in an independent repository and pinned in this
project as the `third_party/cm0-appkit` Git submodule. Launcher compiles AppKit
and its bundled LVGL statically. Its C API supports C and C++ apps, and consumers
use `find_package(LilyGoUI CONFIG REQUIRED)` through the source SDK's CMake
package.

On devices, `lilygo-ui-appkit-dev` 0.1.0 or newer provides the source SDK and complete
Inter, Source Han Sans CN, and Font Awesome runtime fonts and their OFL
licenses. Applications share these font files; AppKit and LVGL code remain in
each application executable. Updating AppKit code requires updating the pinned
submodule and rebuilding the application.

Clone with submodules, or initialize them in an existing checkout:

```sh
git clone --recurse-submodules https://github.com/LILYGO-UI/launcher.git
git submodule update --init --recursive
```

## Project management with LPM

This repository uses [LPM](https://github.com/LILYGO-UI/lpm) as its primary
development interface. Install it once with `uv` (recommended) or `pip`:

```sh
uv tool install "git+https://github.com/LILYGO-UI/lpm.git"
# Or: python3 -m pip install "git+https://github.com/LILYGO-UI/lpm.git"
lpm --version
```

`lpm.toml` is the single developer-maintained source for the Launcher package
name, version, application ID, release metadata, build presets, and deployment
defaults. CMake validates it through LPM during configuration and derives its
project and Debian package metadata from it.

The normal workflow is:

```sh
lpm start                 # Build and start the host simulator in the background
lpm stop                  # Stop the background simulator
lpm test                  # Build and run host tests; write .lpm/test-results.xml
lpm pack                  # Cross-build and write Debian packages to dist/
lpm deploy                # Deploy the newest package to the configured target
lpm metadata --format publish-json --output .lpm/publish.json
```

Use `lpm start --foreground` when simulator logs should remain attached to the
terminal. SSH target defaults live in `[deploy]` in `lpm.toml`; command-line
options such as `--host`, `--user`, `--port`, and `--target` override them.
After `lpm login`, `lpm publish` submits a validated package and its metadata
for review.

## Run the host simulator

Install CMake, pkg-config, SDL2, FreeType, and LPM. On macOS:

```sh
brew install cmake pkg-config sdl2 freetype glib openssl@3
git submodule update --init --recursive
lpm start --foreground
lpm test
```

The host preset selects this repository's AppKit source submodule and builds
its bundled LVGL 9.5.0 statically, without downloading LVGL during configuration.
Host fonts are loaded from the submodule's assets. No installed AppKit binary
SDK or `CMAKE_PREFIX_PATH` setup is needed. If reusing a previous binary-SDK
build directory, reset its CMake cache once with
`cmake --fresh --preset host-simulator` (CMake 3.24+).

Launcher and the independent applications compile LVGL with
`LV_DEF_REFR_PERIOD=20`, matching the CM0 panel's 50 Hz mode. This sets the
default display, animation, and input polling period to 20 ms, including with
older pinned AppKit revisions. Each statically linked executable must be
rebuilt and redeployed for the change to take effect. Actual frame rate still
depends on rendering cost; video playback and emulation retain their own clocks.

Sign in to the host simulator with username `pi` and password `raspberry`.
These virtual credentials support sign-in, screen lock/unlock, and sign-out
without creating or authenticating a system account. Applications run as the
current development-machine user. This authentication is compiled only into
the host simulator; device builds use real Linux accounts and PAM.

The host simulator starts without bundled applications. Install application
packages or point `LAUNCHER_APP_DIRS` at application manifests to populate the
home screen. Each application is built and packaged from its own repository.

Launcher and application logs are written to stderr and are collected by
systemd journald on the device. Applications do not link a launcher logging
library.

The status bar's hardware roots can be overridden for platform integration and
tests with `CM0_POWER_SUPPLY_DIR`, `CM0_INPUT_DIR`, and `CM0_NETWORK_DIR`.

## Launcher UI

`src/launcher_view.cpp` builds the Launcher View directly with the LVGL C API.
`src/ui.cpp` is the C++ Controller: it populates applications from the runtime
registry, connects behavior, and applies orientation changes through typed View
references. The device binary, render test, and host simulator compile the same
sources; there is no XML or UI code-generation step.

The shared status bar and home indicator are implemented once by the
independent AppKit package. Launcher places them in its View, while standalone
applications receive the same system chrome on `lv_layer_top()` from AppKit.

See [`docs/launcher-app-lifecycle.md`](docs/launcher-app-lifecycle.md) for the
complete APP discovery, process launch, display handoff, exit recovery, and
runtime resource flow.

See [`docs/ui-design-guidelines.md`](docs/ui-design-guidelines.md) for the LILYGO UI
responsive-layout and typography requirements, including viewport adaptation,
orientation changes, Inter and Source Han Sans CN usage, type sizes,
mixed-language text, runtime font loading, supplemental fonts, and font
licensing.

## Production startup

The target image does not run a desktop session or compositor. The launcher is
the primary UI and starts as `lilygo-ui-launcher.service` under `multi-user.target`:

```sh
/usr/bin/lilygo-ui-launcher --display drm --drm /dev/dri/card0 \
  --input /dev/input/by-path/platform-3f205000.i2c-event
```

The service is ordered after `network-online.target` and `time-sync.target`, so
the first status-bar reading is taken after the system time synchronizer has had
an opportunity to set `CLOCK_REALTIME`. The image must still enable a time
synchronization provider (such as `systemd-timesyncd` or `chrony`) and configure
the device timezone through `/etc/localtime`; the Launcher follows that system
clock and timezone and does not implement a separate NTP client.

The service opens DRM/KMS and evdev directly. Before starting it, the package
installer disables any configured display manager, masks `getty@tty1.service`,
and changes the default target to `multi-user.target`. It records the previous
target and display-manager state under `/var/lib/lilygo-ui-launcher` so package
removal can restore them. No XDG autostart or application-menu entry is
installed.

The service runs as root to own the display and configure the first account.
For pi-gen images, `lilygo-ui-launcher-first-boot prepare` detects the request
left by `rename-user` (the SSH rename banner, `piwiz.desktop`, or enabled
`userconfig.service`) and saves the original UID and name in the root-owned
`/var/lib/lilygo-ui-launcher/first-boot-pending` file. The Launcher then shows a
mandatory **First-time setup** flow: region and language, account, Wi-Fi,
updates, then completion. The account page has username, password, and confirmation.
The original username is prefilled: keeping `pi` or choosing a new name is
valid. Setup renames the original account, its home, and its matching primary
group in place, preserving the UID, existing supplementary groups, and files
such as preinstalled SSH keys. The image's disabled-login shell is changed to
`/bin/bash`. Exact account entries in `subuid`, `subgid`,
and an existing `010_pi-nopasswd` rule follow the new name.

Setup ensures membership in `sudo`, using the distribution's existing sudo
policy. The package depends on `sudo`; a missing sudo group fails setup before
account changes. Launcher does not create a passwordless rule. An image without
an existing UID 1000 account can instead create its first account with `useradd`,
including `sudo` and the available `video`, `render`, and `input` groups.

After setting the password through `chpasswd` stdin and authenticating through
PAM, Launcher commits `/var/lib/lilygo-ui-launcher/active-user`. The remaining
steps continue before completion removes the upstream wizard entry, temporary wizard account and sudo rule,
SSH rename banner, and autologin marker, and disable `userconfig.service`.
Cleanup leaves the Launcher's DRM service and boot target in place. Failed
setup keeps the original UID as the retry target. A root-only
`first-boot-progress` file records completed steps; after a restart the saved
account must authenticate before setup resumes. Interrupted final cleanup is
retryable. After a rename has started, retries keep the chosen
name until setup completes. Passwords are never saved in Launcher state or
passed on a command line.

With `DISABLE_FIRST_BOOT_USER_RENAME=1` and a preconfigured account, the Launcher
shows normal sign-in instead. The first successful login records that account;
it does not rename it or change its groups. Later boots show sign-in, and
signing out closes the PAM session. Application children run after `initgroups`,
`setgid`, and `setuid` to the authenticated account with its `HOME` and XDG
paths. The systemd `userconfig.service` drop-in prevents the console wizard
from competing for tty1 while Launcher is installed.

After sign-in, open **Quick controls → File sharing** to receive files from a
browser on the same LAN. Select the device named `lilygo_xxxx` (the last two
Bluetooth MAC bytes) in the browser and the
Bluetooth GATT and local WebRTC connection are negotiated automatically; no
pairing code is required. Once the DataChannel opens, the browser releases
Bluetooth and the device stops advertising while the LAN session continues.
Before each file is sent, the device shows its name and size and requires an
explicit **Receive file** confirmation. Declining rejects that file without
receiving its contents. No WSS signaling address or relay service is needed.
Closing the panel, locking, or signing out stops sharing.

Files go to the authenticated user's private `$HOME/Downloads`, using temporary
files, size/SHA-256 verification, atomic commit, and numbered names for conflicts.
The root-owned launcher never uses its inherited `HOME` for this path. See
[browser LAN file sharing](docs/file-sharing.md) for connection steps, browser
requirements, limits, transport dependencies, and tests.

To test setup again on a configured board, preserve its administrator and use a
disposable source account with backed-up wizard state. See the
[re-entry and restoration procedure](tests/README.md#re-entering-setup-on-an-already-configured-board).
The helper's `prepare` command does not reset completed setup; use the
simulator's `--setup-demo` for a UI-only preview.

For an account created by an older Launcher, an existing administrator must
add it once, for example `sudo usermod -aG sudo wing` (or `usermod -aG sudo wing`
from a root shell). Sign out and sign in again, including reconnecting SSH,
before using sudo. Updating Launcher alone does not change existing accounts.

For an interactive pi-gen image, leave `FIRST_USER_PASS` unset and leave
`DISABLE_FIRST_BOOT_USER_RENAME` unset (or set it to `0`). The image does not
ship a default password, and Launcher handles the account step locally without
an XDG desktop. This implements the account lifecycle from
[rename-user](https://github.com/RPi-Distro/userconf-pi/blob/pios/trixie/rename-user),
[userconf](https://github.com/RPi-Distro/userconf-pi/blob/pios/trixie/userconf), and
[cancel-rename](https://github.com/RPi-Distro/userconf-pi/blob/pios/trixie/cancel-rename).
Regional choices come from the installed country, locale, zoneinfo and XKB
catalogs. Launcher applies the country (Wi-Fi regulatory domain), UTF-8 system
language, timezone and system keyboard layout through
[raspi-config](https://github.com/RPi-Distro/raspi-config/blob/trixie/raspi-config)
and checks the saved settings before continuing. Language selection configures
the system locale; the current Launcher UI remains English. Keyboard selection
configures the Linux console and desktop; the Launcher software keyboard and
AppKit's raw evdev text mapping currently retain their US layout.

Wi-Fi uses NetworkManager: scan or enter a hidden SSID, connect with WPA2/WPA3,
Enhanced Open or an open network, or skip. WPA passwords go over stdin, never
argv or Launcher logs. NetworkManager stores the system connection. Enterprise
802.1X and WEP networks require separate administrator configuration.

Updates are optional. Check, install, retry, or return to Wi-Fi after a failure.
The independent `lilygo-ui-launcher-first-boot-update.service` owns apt so a
Launcher package upgrade cannot terminate the installation. It refreshes package
lists, upgrades packages with new dependencies while retaining existing config
files, and records progress under `first-boot-update`. Navigation stays locked
while checking/installing; an interrupted installation can be retried. Finish
enters the home screen; restart after setup when requested by an update.

Preview all steps without changing the host:

```sh
build/host-simulator/lilygo-ui-launcher --setup-demo
```

Browser selection and boot-partition `userconf`/`userconf.txt` import are outside
this flow; preconfigure an account when building a headless image.

Alongside the touchscreen input, the launcher discovers the GPIO `KEY_POWER`
event device by its evdev capabilities. A press sequence shows a shutdown
overlay with `正在关机` and `请继续按住电源键`; the overlay returns to the
normal page after the GEK100 pulse stream has been quiet for 300 ms. The
board's long-press helper handles the three-second power-off decision. Use
`--power-input PATH` when a specific event node is needed for diagnostics;
normal deployments can leave it unset so event numbering and udev path names
remain independent of the launcher.

`lilygo-ui-launcher-console` temporarily unbinds framebuffer console drivers while
the service owns KMS and rebinds them when the service stops. Launcher and
applications use the same DRM device serially. With an AppKit SDK supporting
DRM/EGL handoff, the launcher pauses its timers, completes pending page flips,
drops DRM master and closes evdev input before starting an application. It
keeps its UI, EGL context, texture caches and last framebuffer alive. After the
application exits, it reacquires master, presents the retained frame, reopens
evdev and rebinds gestures before resuming updates. Software DRM, older SDKs
and failed handoffs use display/UI destruction and recreation.

Retaining the launcher caches increases memory use while an application runs;
measure the combined workload on the target. Set `LILYGO_UI_DRM_RECREATE=1`
to force the previous recreation path for diagnosis or comparison. The
`switch prepared` and `switch restored` logs report `release_ms`, `restore_ms`
and whether the display was retained. These are process-side transition
measurements, not application first-frame or physical scanout times. See the
[application lifecycle](docs/launcher-app-lifecycle.md) for ownership, fallback
and diagnostic details. Applications still start as independent new processes.

## Cross-compile for CM0

`cm0-cross` downloads the CM0 BSP SDK from the pinned
[`0.1.0` GitHub release](https://github.com/LILYGO-UI/CM0BspBuilder/releases/download/0.1.0/cm0_sdk.tar.gz)
when it is first configured. CMake verifies the archive's SHA-256 checksum and
extracts it under `.cache/cm0-bsp/0.1.0`; later builds reuse that cache. A local
CM0BspBuilder checkout is not required.

The BSP supplies the compiler and base system libraries. AppKit sources are
compiled from the pinned submodule; the sysroot does not need AppKit binary
packages. For a custom BSP, pass its toolchain with `-DCMAKE_TOOLCHAIN_FILE=...`.
The native file-sharing helper also needs GLib/GIO and OpenSSL development
files. For the pinned BSP, CMake automatically downloads SHA-256-pinned ARM64
development packages into `.cache/cm0-bsp/transport/trixie-arm64-v1`, including
the sysprof and zlib metadata required by GLib. This separate cache reuses the
BSP runtime libraries and leaves the original sysroot untouched. Custom BSPs
must provide their own matching development files. The WebRTC dependencies
are built from pinned source; see
[file sharing](docs/file-sharing.md#device-architecture-and-installed-files).

The device backend has no SDL, Wayland, xkbcommon, spdlog, or fmt dependency.
The default renderer is software. With a BSP containing `libgbm-dev`,
`libegl-dev`, and `libgles-dev`, configure a separate Launcher build with
`-DLILYGO_UI_RENDERER=opengles` to use AppKit's GLES2 texture cache and GPU
display rotation. The pinned BSP 0.1.0 needs those development packages
added to a separate sysroot first; pass that BSP's toolchain directly.
See [AppKit device graphics](third_party/cm0-appkit/README.md#device-graphics)
for configuration and the software/GPU comparison tools. GPU application
packages add `libgbm1`, `libegl1`, and `libgles2` dependencies. EGL initialization
failures are reported at startup; use a software build for devices without a
working EGL/GBM driver.

An experimental `-DLILYGO_UI_RENDERER=nanovg` build uses GPU primitive
drawing through NanoVG GLES2. It requires stencil-8/MSAA-4 EGL support and
must be built in a separate directory. The SDK's graphics comparison runner
tests both static-content scrolling and continuously changing widgets against
software and cached GLES rendering. Keep the production renderer selected
from target measurements rather than changing it merely because a GPU build
succeeds.

The [graphics design](docs/graphics-rendering.md) explains renderer selection,
rotation and buffer ownership, the private VC4 runtime, and the CPU/memory
tradeoffs behind those choices. NanoVG remains experimental because shadow and
arc fidelity differ from the reference. The
[application lifecycle design](docs/launcher-app-lifecycle.md) defines display
retention, fallback recovery, and switch-latency measurement boundaries.

See the [DRM hardware test guide](tests/README.md) for temporary-child exit
coverage, real-application first-frame and return measurements, gesture exits,
and complete-recreation comparisons. The tests require exclusive DRM ownership
on the CM0 target; the guide includes the private VC4 runtime environment used
by the production service. The [archived reports](docs/archive/README.md)
preserve prior hardware results, deployment versions, and recovery evidence;
they do not validate subsequent code changes.
DRM is the production backend; fbdev remains available as a compatibility
fallback.

```sh
lpm pack
file build/cm0-cross/lilygo-ui-launcher
DESTDIR="$PWD/build/cm0-root" cmake --install build/cm0-cross
```

For direct display access, use
`lilygo-ui-launcher --display drm --drm /dev/dri/card0 --drm-connector ID --input
/dev/input/by-path/platform-3f205000.i2c-event`. The stable by-path symlink
avoids depending on the event number assigned during boot. `--display fbdev
--fbdev /dev/fb0` remains available for images that do not use KMS. Add
`--orientation landscape` to start in landscape; Quick controls keeps later
changes for the current Launcher session.

## Application contract

LILYGO UI applications use the `lilygo-ui` package prefix and the
`cc.lilygo.ui` application namespace. For a kebab-case component slug such as
`file-manager`, the Debian package and executable are
`lilygo-ui-file-manager`, while the application ID is
`cc.lilygo.ui.FileManager`. The same application ID is used by its manifest
file and executable descriptor.

An app package installs its executable, discovery manifest, and launcher icon:

```text
/usr/bin/lilygo-ui-hello
/usr/share/launcher/apps/cc.lilygo.ui.Hello.json
/usr/share/icons/hicolor/128x128/apps/cc.lilygo.ui.Hello.png
```

The manifest schema is:

```json
{
  "schema_version": 2,
  "id": "cc.lilygo.ui.Example",
  "name": "Example",
  "description": "Short home-screen text",
  "icon": "../../icons/hicolor/128x128/apps/cc.lilygo.ui.Example.png",
  "order": 100,
  "executable": "../../../bin/lilygo-ui-example"
}
```

`id` must be globally unique and must match the executable's descriptor. `icon`
is an absolute path or a path relative to the manifest. Application packages
install a 128x128 PNG named after their application ID in the hicolor icon
theme. Launcher scales it into the home-screen tile and uses its built-in
default icon when the field is absent, the file is missing, or the image is not
a recognizable PNG. Legacy symbolic icon names remain supported for existing
packages.
Manifests are read from `$LAUNCHER_APP_DIRS`, the user XDG data directory,
`/usr/local/share/launcher/apps`, and `/usr/share/launcher/apps`, in that
precedence order. Invalid manifests, missing executables, and duplicate IDs are
skipped.

Application children run as the authenticated Linux user after `initgroups`,
`setgid`, and `setuid`, with the launcher's root-only environment entries
removed. Launcher does not force `PR_SET_NO_NEW_PRIVS` on that child, so
existing `sudo` or polkit policy can evaluate requests from applications such
as Settings when they change SSH or serial-console configuration. The
application still needs the user's normal authorization and authentication;
Launcher does not provide root credentials.

While the home screen is visible, the directories are rescanned every two
seconds. Newly installed packages appear without restarting the launcher and
removed packages disappear. Each app runs in a child process with its own LVGL
state. The launcher suspends its LVGL timers while the child owns the display,
then redraws the home screen after the child exits.

An app exposes `cm0_app_get_descriptor`, creates its LVGL View under the
supplied `context->root` from `open`, and releases non-LVGL resources from
`close`. An application may load its own LVGL screens because system chrome
lives on the top layer. AppKit's generated `main` owns `lv_init()`,
display/input setup, the event loop, and
`lv_deinit()` in that application process. It also supplies the common status
bar and home indicator. Swiping up from the bottom edge or calling
`context->request_exit()` exits the process. Returning to the home screen is
launcher policy, not an application API.

The launcher and an application can both contain `lv_init()` because they are
different processes with different address spaces and therefore different LVGL
global state. They must not drive the same framebuffer concurrently. The
launcher stops its LVGL timers and display access before the child calls `exec`,
waits for the application process, then resumes and redraws its own display
after the app exits.

Build and package each application from its independent repository root. Its
`cm0-cross` preset should use the same pinned remote-BSP bootstrap rather than a
path into another checkout, and its AppKit dependency should be a pinned Git
submodule:

```sh
git submodule update --init --recursive
cmake --preset cm0-cross
cmake --build --preset cm0-cross --parallel
ctest --test-dir build/cm0-cross --output-on-failure
cpack --config build/cm0-cross/CPackConfig.cmake -G DEB
```

The resulting application package contains only that application and its
optional launcher-discovery manifest. It has no package dependency on
`lilygo-ui-launcher`.

## Build a launcher Debian package

```sh
lpm pack
```

The generated `.deb` is written to the repository's `dist/` directory. Launcher
CPack produces only the `lilygo-ui-launcher` package containing the launcher
executable and systemd unit. Each application repository produces its own `lilygo-ui-*`
package independently; application packages are never components of the launcher
build and do not depend on `lilygo-ui-launcher`.

Installing the launcher package enables `lilygo-ui-launcher.service`, selects
`multi-user.target`, and disables the graphical display manager. The package
contains no desktop-session entry and depends at runtime on `libc6`,
`libstdc++6`, `libdrm2`, `libfreetype6`, the image and compression libraries
used by the AppKit font stack, and `lilygo-ui-appkit-dev` (at least 0.1.0,
as recorded in `project.compatibility.min_appkit_version`). That package
provides the source SDK, shared font files, and licenses. Graphics dependencies follow the
selected renderer. AppKit and LVGL themselves are statically linked, so no
AppKit shared library or ABI capability package is required on the device.
Launcher applications inherit the DRM, connector, evdev, and canvas arguments
required to take over the same display while they run.

See the [SDK and packaging design](docs/sdk-packaging.md) for source and runtime
ownership, package validation, and deployment verification.

The launcher package provides the former `cm0-launcher` identity at the current
version, and conflicts with and replaces that package during migration.

### Store-mediated Launcher updates

Launcher updates must not run `dpkg` in `lilygo-ui-launcher.service`. Store and
all applications launched from it share that service's cgroup, while the old
package's `prerm` stops the service during an upgrade. The package therefore
installs a root-only handoff helper and a separate oneshot service:

```text
/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-update
/usr/lib/systemd/system/lilygo-ui-launcher-update.service
```

After Store has downloaded and verified a published Registry release, it hands
the package to the helper and exits the foreground application:

```sh
pkexec /usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-update \
  queue /tmp/downloaded-launcher.deb VERSION SHA256
```

`queue` copies the package into root-owned storage before it returns and starts
the oneshot service without waiting. When the caller is Store's privileged
installer, the worker first waits for the installer and Store UI to exit so
stopping the old Launcher service cannot terminate Store mid-transaction; a
timeout aborts the update. The service runs outside the Launcher cgroup,
rechecks the SHA-256 and
the Debian `Package`, `Version`, and
`Architecture` fields, rejects reinstalls or downgrades, runs `dpkg`, verifies
the installed version, and only then restarts Launcher. Its latest state is
available from
`/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-update status` and in
`/var/lib/lilygo-ui-launcher/last-update`; full output goes to journald under
`lilygo-ui-launcher-update.service`.

Store must treat `lilygo-ui-launcher` as a non-removable system package. It may
offer only the update action for this package, and must use the handoff helper
instead of its normal synchronous `dpkg --install` path. The Debian package is
also marked with `Priority: required`, and its `prerm` rejects removal unless an
administrator explicitly sets `LILYGO_UI_LAUNCHER_ALLOW_REMOVE=1`. The helper
is the upgrade path after a baseline package containing it has been installed;
images with an older Launcher require that baseline package to be deployed once
by the system image or an administrator.

Deploy the newest generated package using the target configured in `lpm.toml`:

```sh
lpm deploy
```

During a cold boot, another boot client can briefly retain DRM master after the
device node and connector are already available. The device DRM backend waits
up to ten seconds to acquire master before initializing KMS. Its first atomic
modeset remains retryable until a commit succeeds, and DRM warnings and errors
are forwarded to the service journal.

### K230 Debian riscv64 with LPM

Install `gcc-riscv64-linux-gnu` and `g++-riscv64-linux-gnu` on Linux, or LLVM, LLD and dpkg on macOS (`brew install llvm lld dpkg`). From this application's repository, run the packaging command.

```sh
lpm pack --arch riscv64
```

The `k230-debian` CMake preset downloads the pinned Debian BSP from the `LILYGO-UI/canaan-gen` release and verifies its SHA-256. It keeps the archive and `sysroot/` in this project's `.cache/k230-bsp/20261116T104343Z/`. The next build reuses the cache offline, even with a new `build/` directory. Packages appear in `dist/`.

Run `lpm set-target k230` once to use `lpm pack` for subsequent builds. To use your own Debian sysroot, set `LILYGO_UI_DEBIAN_SYSROOT` before the first configure, or pass `-DLILYGO_UI_DEBIAN_SYSROOT=/path/to/sysroot` to CMake. The pinned `third_party/cm0-appkit` supplies the application SDK. `-DLilyGoUI_DIR=/path/to/appkit` selects another copy.
