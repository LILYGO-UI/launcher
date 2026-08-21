# LILYGO UI Launcher

LILYGO UI Launcher is an LVGL application launcher for LILYGO Linux products.
CM0 is the first supported product profile. The current device
build uses Linux DRM/KMS and evdev directly, with fbdev retained as a fallback;
the host build uses an SDL2 window. Installed applications are independent executables discovered from JSON
manifests, so installing or removing a compatible `.deb` changes the home
screen without rebuilding the launcher.

The device and simulator default to the product UI's 568x1232 portrait canvas.
The home screen uses a 96 px AppKit status bar and shows up to 18 applications per
page in a 3x6 portrait grid or 6x3 landscape grid. Swipe down from the status
bar to open Quick controls and switch orientation. Swipe horizontally or use
the page indicators to reach additional applications. On the host, the
simulator automatically scales its window to fit the current display while
preserving that logical canvas. The SDL host window uses high-DPI rendering so
the scaled interface remains crisp.
`--orientation portrait|landscape`, `--width`, and `--height` remain available
for diagnostics.

## Project structure

```text
src/
  *.cpp, *.h  Launcher runtime and UI sources
tests/        Test and UI-rendering sources
assets/
  images/     Image assets
  audio/      Audio assets
  fonts/      Font assets
```

Launcher sources live directly under `src/` and launcher tests under `tests/`.
Applications live in independent repositories and are not stored, compiled,
tested, or packaged by the launcher repository.

LILYGO UI AppKit is maintained in an independent repository and pinned in this
project as the `third_party/cm0-appkit` Git submodule. It is compiled statically
into the Launcher and every application. It exposes a small C API, so an app
may be implemented in either C or C++. Consumers use its CMake package with
`find_package(LilyGoUI CONFIG REQUIRED)` and never include AppKit files
through a relative path into the Launcher repository.

Clone with submodules, or initialize them in an existing checkout:

```sh
git clone --recurse-submodules https://github.com/LILYGO-UI/launcher.git
git submodule update --init --recursive
```

## Run the host simulator

Install CMake, pkg-config, and SDL2. On macOS:

```sh
brew install cmake pkg-config sdl2
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
./build/host-simulator/lilygo-ui-launcher
```

AppKit carries its pinned LVGL 9.5.0 source and builds it statically, so the
simulator does not download LVGL during configuration.
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
mixed-language text, embedded glyph generation, and font licensing.

## Production startup

The target image does not run a desktop session or compositor. The launcher is
the primary UI and starts as `lilygo-ui-launcher.service` under `multi-user.target`:

```sh
/usr/bin/lilygo-ui-launcher --display drm --drm /dev/dri/card0 \
  --input /dev/input/by-path/platform-3f205000.i2c-event
```

The service opens DRM/KMS and evdev directly. Before starting it, the package
installer disables any configured display manager, masks `getty@tty1.service`,
and changes the default target to `multi-user.target`. It records the previous
target and display-manager state under `/var/lib/lilygo-ui-launcher` so package
removal can restore them. No XDG autostart or application-menu entry is
installed.

`lilygo-ui-launcher-console` temporarily unbinds framebuffer console drivers while
the service owns KMS and rebinds them when the service stops. Launcher and
applications use the same DRM device serially: the launcher releases its DRM
display and evdev input before starting an application, then reacquires both
after that process exits.

## Cross-compile for CM0

`cm0-cross` downloads the CM0 BSP SDK from the pinned
[`0.1.0` GitHub release](https://github.com/LILYGO-UI/CM0BspBuilder/releases/download/0.1.0/cm0_sdk.tar.gz)
when it is first configured. CMake verifies the archive's SHA-256 checksum and
extracts it under `.cache/cm0-bsp/0.1.0`; later builds reuse that cache. A local
CM0BspBuilder checkout is not required.

The device backend has no SDL, Wayland, xkbcommon, spdlog, or fmt dependency.
DRM is the production backend; fbdev remains available as a compatibility
fallback.

```sh
cmake --preset cm0-cross
cmake --build --preset cm0-cross --parallel
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
cd build/cm0-cross
cpack -G DEB
```

The generated `.deb` is written to the repository's `dist/` directory. Launcher
CPack produces only the `lilygo-ui-launcher` package containing the launcher
executable and systemd unit. Each application repository produces its own `lilygo-ui-*`
package independently; application packages are never components of the launcher
build and do not depend on `lilygo-ui-launcher`.

Installing the launcher package enables `lilygo-ui-launcher.service`, selects
`multi-user.target`, and disables the graphical display manager. The package
contains no desktop-session entry and depends only on libc, libstdc++, and
libdrm at runtime. Launcher applications inherit the DRM, connector, evdev, and
canvas arguments required to take over the same display while they run.

The launcher package provides, conflicts with, and replaces the former
`cm0-launcher` package during migration.

During a cold boot, another boot client can briefly retain DRM master after the
device node and connector are already available. The device DRM backend waits
up to ten seconds to acquire master before initializing KMS. Its first atomic
modeset remains retryable until a commit succeeds, and DRM warnings and errors
are forwarded to the service journal.
