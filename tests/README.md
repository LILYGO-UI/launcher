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

## Browser LAN file sharing

The implemented connection is Bluetooth GATT signaling plus an ordered,
reliable LAN WebRTC DataChannel. Bluetooth bootstraps the connection and is
released after the complete SDP answer and DataChannel establishment; file
transfers then continue independently over the LAN. It uses no WSS server or
STUN/TURN relay.
Protocol details and user steps are in
[the file-sharing guide](../docs/file-sharing.md).

Build the host suite and run the relevant checks:

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator
ctest --preset host-simulator -R 'launcher-(file-share|transport|bluez-agent|render-ui)'
```

- `launcher-file-share` checks private Downloads ownership/mode, exact bytes,
  size/hash rejection, ordering/limits, cancellation, conflict policies,
  symlink rejection, and long-extension/UTF-8 auto-renaming.
- `launcher-file-share-receiver` launches the actual C++ IPC worker and a
  deterministic C++ peer. It checks split stream frames, begin/data/finish,
  empty files, auto-renaming, invalid commands/metadata/oversized frames,
  failures and cancellation, connection-loss cleanup, inherited-descriptor
  closure, and bounded termination of a helper that ignores SIGTERM.
- `launcher-transport` checks native BLE message framing, malformed/order/size
  rejection, storage IPC, automatic connection, per-file approval, and the file protocol. It also creates real
  libdatachannel peers for local WebRTC transfer checks.
- `launcher-bluez-agent` uses a private D-Bus daemon and a native mock BlueZ
  service to exercise the actual helper executable without a physical adapter.
  A host without `dbus-daemon` reports a skip; Linux target validation must run
  this check with the daemon installed.
- `launcher-render-ui` verifies the status, directory, file details, and
  Receive file/Decline/Close controls at 568×1232 and 320×240, including
  keyboard decisions with Decline selected for each new request. It writes
  `launcher-file-sharing.ppm` and `launcher-file-sharing-compact.ppm` alongside
  the existing rendering outputs.

Transport sources, fixtures, and tests are C++. Build dependencies and a
standalone transport build command are in the file-sharing guide.
On macOS, install `dbus` with Homebrew to run the private-bus integration test.

The independent browser repository `lilygo-ui.github.io` has its own checks.
Run `npm test` with Node 22.6 or newer for browser protocol/state-machine tests,
and `npm run check` for those tests plus the VitePress build. Launcher neither
builds nor packages the browser repository.

### Opt-in target harness

`launcher-file-share-target` uses the production C++ receiver and storage with
an explicitly selected existing account. It is available when `BUILD_TESTING`
is enabled, including cross builds; it is never installed or registered as an
automatic CTest. Build it without enabling the rest of the host-only suite:

```sh
cmake --preset cm0-cross -DBUILD_TESTING=ON
cmake --build --preset cm0-cross --target launcher-file-share-target
cmake --preset cm0-cross -DBUILD_TESTING=OFF
```

Copy the harness and `lilygo-ui-file-share-agent` executables to an administrator-owned
test directory on the configured target. Use `.target-board.env` for the
connection settings, without printing its password. Install the transport
dependencies if absent:

```sh
sudo apt-get install bluez libglib2.0-0t64 libssl3t64
```

Run on the board with a real non-root account; replace `pi` with the account
being tested:

```sh
sudo /root/lilygo-share-test/launcher-file-share-target \
  --user pi \
  --agent /root/lilygo-share-test/lilygo-ui-file-share-agent \
  --duration 300
```

The harness prints changed receiving status. It does not auto-approve files.
It exits on SIGINT, SIGTERM, or the duration limit and fails if the transport
exits unexpectedly. Keep Launcher sharing closed while using the harness so
only one owner advertises the service.

For a complete browser-to-board check, use the installed Launcher panel, open
the browser sharing page over HTTPS/localhost, select the device named
`lilygo_xxxx` (the last two Bluetooth MAC bytes), and
wait for the direct connection to establish automatically. Send a file
larger than 1 MiB, an empty file, a Chinese filename, and a repeated filename.
Verify the saved bytes/SHA-256, numeric duplicate suffix, owning UID, `0600`
file mode, and `0700` Downloads mode. Before approving each file on the device,
check its displayed name and size and verify that no file content or temporary
destination file has been created. Decline one request, cancel another while
approval is pending, then approve a fresh request to check that approval cannot
be reused. Verify that the browser's automatic GATT
release and device advertisement withdrawal leave the established DataChannel
usable for subsequent files. Bluetooth disconnection during negotiation should
abort setup. Cancel a partial transfer and close WebRTC during another; neither
may leave a final file or `.lilygo-transfer-*.tmp`.
Check panel close, lock, logout, and application handoff cleanup. These manual checks provide radio,
browser, and signed-in lifecycle evidence beyond the host simulations.

## Host-only checks

The normal Launcher CTest suite includes `launcher-display-session`,
`launcher-process-identity`, and `launcher-session`. It compiles
the production display controller with mock devices in three configurations:
software, EGL with handoff, and EGL with an older SDK lacking handoff APIs.
It checks retained and recreated handoffs, recovery failures, input rebinding,
and cleanup after external deletion. Keyboard cases cover TCA8418 connection
rotation, disconnects preserving direction, manual rotation with or without a
keyboard, and retaining manual choices across application handoffs. The
`launcher-keyboard-device` test uses temporary sysfs fixtures and simulated I2C
responses to distinguish TCA8418 devices and detect disconnects even when their
kernel device nodes remain. It checks driver reinitialization after a chip reset,
binding retries, and preservation of healthy or unrelated driver bindings.
Display cases cover replacement evdev discovery after recovery, preservation of
USB keyboard ownership, and recovery retries without changing manual orientation.
The process test runs real child processes to verify that periodic maintenance
continues while the child runs, including normal exit and TERM/KILL handling.
The identity test verifies UID/GID drops,
XDG environment construction, removal of root-only variables, and preservation
of only the exact paired private Mesa runtime paths; the session
test verifies first-use marker validation and the host build's refusal to create
accounts. This checks ownership and control flow;
it does not replace the target tests above.

`launcher-session-pam` uses mock PAM transactions to verify that successful and
failed unlock attempts preserve the signed-in user's session and credentials.
It also intercepts account helpers to verify disabled-wizard sign-in, a pending
locked/nologin account keeping its name or being renamed with the same UID,
sudo membership, password delivery through stdin, exact account mapping
updates, collision checks, and retries after helper/PAM/marker/cleanup failures.
The partial-home case models passwd changing before `usermod` moves the directory.
`launcher-first-boot` uses a fixture root and mocked system commands to verify
CLI/desktop trigger detection, completion-only cleanup, retry after interrupted
cleanup, retaining the upstream request while full-wizard progress exists, and
rejection of unsafe state. These tests never alter host accounts.
`launcher-account-view` checks the fixed-user lock form, password clearing,
the prefilled first-use form, retained-name submission, error retries, busy
feedback, keyboard focus isolation, hotplug input capture, and layout changes.
`launcher-render-ui` checks lock-button activation, blocked home actions while
locked, and Quick controls geometry in portrait and compact landscape.

The complete first-boot flow adds:

- `launcher-first-boot-wizard`: a simulator end-to-end LVGL test for region →
  account → Wi-Fi → update → finish, optional-step skips, returning to Wi-Fi,
  failed password validation, and keeping all host account state untouched.
- `launcher-setup-view`: portrait/landscape/compact layouts, focus isolation,
  hardware keyboard input, Wi-Fi field clearing and busy/update actions. Pass a
  directory to this test executable to render all four pages as PPM previews.
- `launcher-setup-region`: distro catalog fixtures, language defaults, timezone
  symlinks, keyboard variant reset, helper failures and saved-state validation.
- `launcher-setup-network`: nmcli escaping, network/security selection,
  password validation and stdin-only delivery, cleanup of failed connections.
- `launcher-setup-update` / `launcher-setup-update-helper`: status parsing,
  check/install/failure/retry, dpkg recovery and a separately owned update job.
  Set `LILYGO_TEST_REAL_FLOCK=1` when running the shell test inside Linux to
  exercise a live worker lock while status requests and duplicate starts arrive.
- `launcher-setup-command` / `launcher-setup-flow`: real pipes, SIGPIPE, large
  stdin/output, timeout, argv/environment isolation, durable atomic progress,
  restart/resume rules and unsafe-state rejection.
- `launcher-process-locale`: safe parsing of modern/legacy system locale files
  for launched device apps, while simulator children retain the host locale.

Target acceptance still requires a fresh image with the wizard enabled: select
regional values, create an account, join the intended access point, check and
install updates, and verify recovery after a Launcher restart. Confirm the new
account's `sudo`, locale, timezone and console keyboard layout after signing in.
The automated Wi-Fi tests use a command fixture, not a radio or access point.
The automated update tests use apt fixtures, not a full live OS upgrade.

### Re-entering setup on an already configured board

For a UI-only preview, use `build/host-simulator/lilygo-ui-launcher --setup-demo`.
The device build does not accept this option. The first-boot helper's `prepare`
command preserves completed setup; it is not a reset command. Removing only
`active-user` also does not re-enable setup when a configured UID 1000 exists.

For a real repeatable test, keep the existing administrator (for example `pi`)
and its SSH session unchanged. Use a separate disposable source account, which
the wizard will rename and grant sudo membership. Do not point the pending
marker at `pi`, and do not run the distribution's `rename-user` to reset a
configured board. First ensure that the update service has no active or queued
job and that apt/dpkg are idle. Use unused account/group names for both
`lilygo-wiz-source` and `lilygo-wiz-check`; ensure their home paths are absent.
The example assumes the board has the `video`, `render` and `input` groups.

Run the following in one root shell, keeping the printed backup path for
restoration. The state directory must already be root-owned mode 0700. Only the
four wizard state entries are moved; the package's boot/display recovery state
is left in place.

```sh
sudo -i
set -eu
state=/var/lib/lilygo-ui-launcher
systemctl stop lilygo-ui-launcher.service
backup=$(mktemp -d /root/lilygo-wizard-retest.XXXXXX)
printf 'Restore from: %s\n' "$backup"
mkdir -m 0700 "$backup/state"
useradd -m -U -s /usr/sbin/nologin -G video,render,input lilygo-wiz-source
id -u lilygo-wiz-source > "$backup/test-uid"
for item in active-user first-boot-pending first-boot-progress first-boot-update; do
    if [ -e "$state/$item" ] || [ -L "$state/$item" ]; then
        mv "$state/$item" "$backup/state/$item"
    fi
done
umask 077
printf '%s:lilygo-wiz-source\n' "$(cat "$backup/test-uid")" > "$state/first-boot-pending"
chmod 0600 "$state/first-boot-pending"
sync -f "$state"
systemctl start lilygo-ui-launcher.service
```

In the account page, choose `lilygo-wiz-check` and enter a temporary password
privately. The source starts with a locked password, nologin shell and no sudo
membership. Verify that setup preserves its recorded UID, moves its home,
enables `/bin/bash` and adds sudo. After a fresh login as the test account, run
`sudo -k` followed by `sudo id -u`; enter its password and expect `0`.

To restore the original account selection, wait for any update to finish, stop
Launcher, and close only the test account's sessions (`loginctl terminate-user
lilygo-wiz-check` if needed). Move the current four wizard state entries into a
separate `result` directory under the same backup, then move the saved entries
from `$backup/state` back into the Launcher state directory. Verify that the
restored `active-user` names the original account, such as `pi`. Resolve the
current test account using the saved UID and verify its name and home before
`userdel --remove`; it may still be named `lilygo-wiz-source` after an aborted
test. Remove only any leftover private test group, preserve the root-only
results for diagnostics, sync the state directory, then start Launcher. Never
restore `/etc/passwd` or `/etc/shadow` wholesale, delete the original account,
or remove the whole Launcher state directory.

This is a real system test: regional settings, Wi-Fi connections and installed
updates persist independently of the wizard markers. Record the previous
locale, timezone, keyboard (including its variant), Wi-Fi regulatory domain and
active NetworkManager connection UUID first. Use a stable management connection
or an arranged network rollback when testing Wi-Fi; switching its profile can
disconnect SSH. Restore the original network before removing only the test
profile. An apt upgrade is not undone by restoring wizard state. Completion
also retires the upstream piwiz/userconfig request and temporary
`rpi-first-boot-wizard` account, so inspect those before using this procedure on
a board with unfinished upstream setup.

### Running the host and container checks

```sh
git submodule update --init --recursive
cmake --preset host-simulator
cmake --build --preset host-simulator
ctest --preset host-simulator
```

`test_first_boot_linux.cpp` and `test_first_boot_linux.sh` additionally exercise
real Linux account helpers and PAM inside disposable Docker containers. They
are intentionally excluded from CTest because they create and rename accounts.
Use a local Debian 13 image with `passwd` and PAM (the local `pi-gen:latest`
image works), compile the driver for that image's architecture, and bind the
repository read-only. The following cross-build uses the same cached sysroot
as `cm0-cross`:

```sh
mkdir -p build/first-boot-integration
test_sysroot="$PWD/.cache/cm0-bsp/0.1.0/sysroot"
aarch64-linux-gnu-g++ --sysroot="$test_sysroot" \
  -isystem "$test_sysroot/usr/include/aarch64-linux-gnu" \
  -std=c++17 -Wall -Wextra -Wpedantic -DLAUNCHER_SYSTEM_SESSION=1 -Isrc \
  -B"$test_sysroot/usr/lib/aarch64-linux-gnu/" \
  -B"$test_sysroot/usr/lib/gcc/aarch64-linux-gnu/14/" \
  -Wl,-rpath-link,"$test_sysroot/usr/lib/aarch64-linux-gnu" \
  -L"$test_sysroot/usr/lib/aarch64-linux-gnu" \
  tests/test_first_boot_linux.cpp src/session.cpp src/setup_flow.cpp \
  "$test_sysroot/usr/lib/aarch64-linux-gnu/libpam.so.0" \
  -o build/first-boot-integration/driver
for scenario in rename keep resume resume-home cleanup disabled full; do
  docker run --rm --network none --env LILYGO_FIRST_BOOT_DISPOSABLE_TEST=1 \
    --mount type=bind,source="$PWD",target=/source,readonly \
    --entrypoint /bin/sh pi-gen:latest /source/tests/test_first_boot_linux.sh \
    /source/build/first-boot-integration/driver /source "$scenario"
done
```

The enabled-wizard fixtures start with UID 1000, a locked password, and
`/usr/sbin/nologin`, matching pi-gen without `FIRST_USER_PASS`. They check the
home and SSH files, memberships, account mappings, real password authentication,
completion state, and sign-in after a fresh process starts. If the container has
`sudo`, they also execute sudo with the inherited policy and then with the
standard password-authenticated sudo-group policy. Only systemctl is mocked,
because these containers do not boot systemd.
The `full` scenario commits the account with deferred cleanup, starts a fresh
process to authenticate and resume the Network stage, then completes both the
upstream cleanup and the durable wizard progress.

These commands check Python syntax and CLI availability without opening DRM
or launching applications:

```sh
python3 -m py_compile tests/drm_test_support.py tests/test_drm_runtime.py \
  tests/test_app_drm_runtime.py
python3 tests/test_drm_runtime.py --help
python3 tests/test_app_drm_runtime.py --help
```
