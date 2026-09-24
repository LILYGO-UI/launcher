# Launcher DRM hardware checks

The scripts in this directory are opt-in target tests for a CM0 with its
568x1232 panel. They are separate from host CTest and require exclusive DRM
ownership. Run them from the Launcher checkout on the target, or copy both
scripts and `drm_test_support.py` into the same directory. Python's standard
library is sufficient.

The CM0 VC4 optimization uses the pinned AppKit source submodule with
`LILYGO_UI_RENDERER=opengles` and retains the
Launcher's UI, EGL context and framebuffer during application handoff.
The default software build recreates the display; select test expectations
from the actual build and startup log. See the
[graphics design](../docs/graphics-rendering.md) and
[lifecycle design](../docs/launcher-app-lifecycle.md) for the design constraints.
`LILYGO_UI_DRM_RECREATE=1` forces complete recreation for diagnosis. NanoVG is
an experimental renderer with known shadow/arc differences; passing these
lifecycle checks does not establish equivalent visual output.

## Running on the target

Stop `lilygo-ui-launcher.service` before a test and restart it after the test,
including after a failure. The scripts themselves do not manage services or
install packages. Use the device's connector ID; `44` below is an example.

```sh
sudo systemctl stop lilygo-ui-launcher.service
sudo python3 tests/test_drm_runtime.py /usr/bin/lilygo-ui-launcher \
  --connector 44 --renderer opengles --cycles 3
sudo systemctl start lilygo-ui-launcher.service
```

The smoke test launches and returns from a temporary child `--cycles` times
in each of portrait and landscape, then checks clean shutdown. Landscape
returns exercise software rotation-buffer allocation when rebuilding the
display. Repeat with `--child failure`, `--child signal`, and
`--child exec-failure` to cover abnormal application exits. It prints complete
logs, including on failure. Its log poll interval is 50 ms and it does not
write latency metrics.

The real-application test launches each selected application in both
orientations. It isolates XDG state, gives Files an empty directory, and
directs Meshtastic to loopback port 9. To measure completed first frames and
SIGTERM returns:

```sh
sudo python3 tests/test_app_drm_runtime.py \
  --launcher /usr/bin/lilygo-ui-launcher \
  --launcher-renderer opengles \
  --app /usr/bin/lilygo-ui-settings \
  --app /usr/bin/lilygo-ui-files \
  --app /usr/bin/lilygo-ui-meshtastic \
  --connector 44 --cycles 3 --require-app-ready \
  --metrics /tmp/lilygo-ui-switch-sigterm.jsonl
```

Match `--renderer` in the smoke test and `--launcher-renderer` in the real-app
test to the Launcher's build. Use `software` for a software-rendered Launcher;
the real-app test still requires each application to report the VC4 GPU.
The expected Launcher renderer is also recorded in real-app metrics.

Run it again with `--exit-method gesture` and a different `--metrics` path to
check the bottom-edge exit gesture. Match `--app-rotation` to the AppKit source
used to build the application: `--app-rotation 90` (the default) for
`LV_DISPLAY_ROTATION_90`, or `--app-rotation 270` for
`LV_DISPLAY_ROTATION_270`. This option
controls the application's swipe coordinates; Launcher taps always use its
90-degree landscape rotation. Portrait coordinates are unchanged. Metrics
record the selected application rotation. Metrics files are overwritten when a run
starts; completed cycles remain available if a later cycle fails. Without
`--require-app-ready`, readiness may mean only GPU context initialization.

For complete-recreation coverage, prefix either test command with
`sudo env LILYGO_UI_DRM_RECREATE=1 python3` in place of `sudo python3`.
The production service's environment is not inherited by a manually launched
test. On a target with the private VC4 runtime installed, reproduce that
configuration by adding these environment values after `sudo env`:

```sh
LD_LIBRARY_PATH=/usr/lib/lilygo-ui/mesa-vc4/lib \
GBM_BACKENDS_PATH=/usr/lib/lilygo-ui/mesa-vc4/lib/gbm
```

These settings apply only to the test process and its children. See the
[graphics design](../docs/graphics-rendering.md) and
[standalone Mesa VC4 repository](https://github.com/LILYGO-UI/mesa-vc4#readme) for the
private runtime's build and deployment details.

## Interpreting results

Real-application timing uses a monotonic clock and 10 ms log polling. Entry
starts immediately before submitting the simulated tap release; return starts
at SIGTERM submission or the beginning of the exit gesture. The gesture path
keeps observing readiness while the swipe is in progress, since AppKit may
exit before the pointer is released. `app_reported_first_frame_ms` is AppKit's
own duration and has a different origin from the host-observed entry metric.

The scripts check process lifecycle, software readiness markers and renderer
selection. They do not measure physical panel scanout, input-to-photon latency,
frame rate or visual fidelity. RSS and MemAvailable are sampled after readiness
and are not complete GPU memory accounting. Keep renderer, Mesa runtime, app
inputs and exit method consistent when comparing runs.

The [renderer comparison](../docs/archive/graphics-validation-2026-09-17.md) and
[switch latency report](../docs/archive/app-switch-latency-2026-09-17.md) record prior
hardware runs, including their limitations and artifact locations. They are
historical evidence, not validation of subsequent code changes.

## Host-only checks

The normal Launcher CTest suite includes `launcher-display-session`. It compiles
the production display controller with mock devices in three configurations:
software, EGL with handoff, and EGL with an older SDK lacking handoff APIs.
It checks retained and recreated handoffs, recovery failures, input rebinding,
and cleanup after external deletion. This checks ownership and control flow;
it does not replace the target tests above.

```sh
git submodule update --init --recursive
cmake --preset host-simulator
cmake --build --preset host-simulator
ctest --preset host-simulator
```

These commands check Python syntax and CLI availability without opening DRM
or launching applications:

```sh
python3 -m py_compile tests/drm_test_support.py tests/test_drm_runtime.py \
  tests/test_app_drm_runtime.py
python3 tests/test_drm_runtime.py --help
python3 tests/test_app_drm_runtime.py --help
```
