#!/usr/bin/env python3
r"""Opt-in real-app DRM handoff test for a CM0 568x1232 panel.

Stop the installed Launcher service before running, and restart it afterwards:
  sudo python3 tests/test_app_drm_runtime.py --launcher /path/to/launcher \
    --app /path/to/lilygo-ui-settings --app /path/to/lilygo-ui-files \
    --app /path/to/lilygo-ui-meshtastic --connector 44

Each app is launched from a temporary Launcher manifest in both orientations.
Synthetic FIFO input selects the Launcher's first tile and optionally performs
AppKit's bottom-edge exit gesture. Files uses an empty temporary directory,
Meshtastic uses loopback port 9, and XDG directories are isolated. The app must
remain alive, report the VC4 renderer, exit cleanly by SIGTERM or gesture, and
return DRM to Launcher. --launcher-renderer selects the expected Launcher
renderer; software does not require a Launcher GPU context.
This checks lifecycle and renderer selection, not physical pixels or frame rate.
Full logs are printed, including on failure. No services or packages are changed.

Use --require-app-ready to wait for AppKit's first completed frame, rather than
only GPU initialization. --metrics PATH writes one JSON object per successful
switch cycle. Host-observed durations use a monotonic clock and 10 ms log polls;
they include input handling, process startup and display restoration, but do not
measure physical panel scanout. An app's own first_frame_ms is recorded separately.
Metrics also sample both processes' RSS and system MemAvailable once at app
readiness, after the latency timestamp; these are not GPU allocation totals.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile
import time

from drm_test_support import (
    POLL_SECONDS, input_fifo, physical_position, position, read_log,
    ready_marker, sync, tap_first_tile, touch, wait_for,
)


APP_IDS = {
    "lilygo-ui-settings": "cc.lilygo.ui.Settings",
    "lilygo-ui-files": "cc.lilygo.ui.Files",
    "lilygo-ui-meshtastic": "cc.lilygo.ui.Meshtastic",
    "lilygo-ui-template": "cc.lilygo.ui.Template",
}
ERROR = re.compile(
    r"\[error\]|EGL_BAD_|GL_(?:INVALID|OUT_OF_MEMORY)|"
    r"cannot (?:initialize|restore|execute)|Segmentation fault|"
    r"Assertion .*failed", re.IGNORECASE)
GPU = re.compile(r"OpenGL ES renderer=[^\n]*\bVC4\b")
APP_READY = re.compile(r"\[appkit\] UI ready\b[^\n]*")
FIRST_FRAME_MS = re.compile(r"\bfirst_frame_ms=(\d+(?:\.\d+)?)(?=\s|$)")


def app_ready_duration(text, app_id):
    identity = re.compile(r"\bapp_id=" + re.escape(app_id) + r"(?=\s|$)")
    for match in APP_READY.finditer(text):
        line = match.group()
        duration = FIRST_FRAME_MS.search(line)
        if identity.search(line) and duration:
            return float(duration.group(1))
    return None


def elapsed_ms(start_ns, end_ns):
    return round((end_ns - start_ns) / 1_000_000, 3)


def proc_kib(path, field):
    try:
        for line in path.read_text().splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[0] == field + ":" and parts[2] == "kB":
                return int(parts[1])
    except (OSError, ValueError):
        pass
    return None


def sample_memory(launcher_pid, app_pid):
    return {
        "launcher_rss_kib": proc_kib(Path(f"/proc/{launcher_pid}/status"), "VmRSS"),
        "app_rss_kib": proc_kib(Path(f"/proc/{app_pid}/status"), "VmRSS"),
        "mem_available_kib": proc_kib(Path("/proc/meminfo"), "MemAvailable"),
    }


def child_is_alive(pid, launcher, executable):
    try:
        # The app exec wrapper preserves the PID reported by Launcher.
        stat = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return (stat[0] != "Z" and int(stat[1]) == launcher.pid
                and Path(f"/proc/{pid}/exe").resolve() == executable)
    except (FileNotFoundError, ProcessLookupError):
        return False


def swipe_exit(fd, orientation, app_rotation, observe_return):
    width, height = (568, 1232) if orientation == "portrait" else (1232, 568)
    logical_x = width // 2
    start_y = height - 8
    end_y = height - max(160, height // 4)

    def move(logical_y):
        position(fd, *physical_position(logical_x, logical_y, orientation, app_rotation))

    started_ns = time.monotonic_ns()
    move(start_y)
    touch(fd, True)
    sync(fd)
    for step in range(1, 7):
        # Keep observing while input is held: AppKit may exit as soon as the
        # distance threshold is crossed, before all six steps have been sent.
        for _ in range(3):
            time.sleep(POLL_SECONDS)
            observe_return()
        move(start_y + (end_y - start_y) * step // 6)
        sync(fd)
    touch(fd, False)
    sync(fd)
    observe_return()
    return started_ns


def run_app(args, executable, directory):
    app_id = APP_IDS[executable.name]
    app_dir = directory / "apps"
    app_dir.mkdir()
    files_root = directory / "files"
    files_root.mkdir()
    environment = os.environ.copy()
    for key, leaf in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                      ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state")):
        target = directory / leaf
        target.mkdir(mode=0o700)
        environment[key] = str(target)

    command = [str(executable)]
    if executable.name == "lilygo-ui-files":
        command += ["--root", str(files_root)]
    elif executable.name == "lilygo-ui-meshtastic":
        command += ["--host", "127.0.0.1", "--port", "9"]
    wrapper = directory / "app-exec"
    wrapper.write_text(
        "#!/bin/sh\nexec " + shlex.join(command) + ' "$@"\n')
    wrapper.chmod(0o700)
    (app_dir / f"{app_id}.json").write_text(json.dumps({
        "schema_version": 2, "id": app_id, "name": "Graphics Test",
        "icon": "settings", "order": -2147483648,
        "executable": str(wrapper),
    }))
    fifo = directory / "input"
    with input_fifo(fifo) as fd:
        for orientation in ("portrait", "landscape"):
            log = directory / f"{orientation}.log"
            child_pid = None
            with log.open("w") as output:
                process = subprocess.Popen(
                    [str(args.launcher), "--display", "drm", "--drm", args.drm,
                     "--drm-connector", str(args.connector), "--input", str(fifo),
                     "--app-dir", str(app_dir), "--orientation", orientation],
                    stdout=output, stderr=subprocess.STDOUT, env=environment,
                )
                try:
                    ready = ready_marker(orientation)
                    text, _ = wait_for(process, log, lambda t: ready in t,
                                       f"{orientation} Launcher startup", error_pattern=ERROR)
                    if f"(renderer={args.launcher_renderer})" not in text:
                        raise RuntimeError(
                            f"Launcher did not report renderer={args.launcher_renderer}")
                    if args.launcher_renderer != "software" and not GPU.search(text):
                        raise RuntimeError("Launcher did not report the VC4 GPU")
                    started = re.compile(r"started " + re.escape(app_id) + r" as process (\d+)")
                    exited = f"{app_id} exited with code 0"
                    for cycle in range(1, args.cycles + 1):
                        released_ns = tap_first_tile(fd, orientation)
                        text, started_ns = wait_for(
                            process, log, lambda t: len(started.findall(t)) >= cycle,
                            f"{app_id} launch {cycle}", error_pattern=ERROR)
                        match = list(started.finditer(text))[-1]
                        child_pid = int(match.group(1))
                        offset = match.end()

                        def initialized(t):
                            if f"{app_id} exited with code" in t[offset:]:
                                raise RuntimeError("App exited before completing startup")
                            return (GPU.search(t[offset:]) and child_is_alive(
                                child_pid, process, executable)
                                and (not args.require_app_ready
                                     or app_ready_duration(t[offset:], app_id) is not None))

                        readiness = "first frame" if args.require_app_ready else "VC4 startup"
                        text, app_observed_ns = wait_for(
                            process, log, initialized, f"{app_id} {readiness}",
                            error_pattern=ERROR)
                        memory = sample_memory(process.pid, child_pid) if args.metrics else {}
                        first_frame_ms = app_ready_duration(text[offset:], app_id)
                        deadline = time.monotonic() + args.seconds
                        while time.monotonic() < deadline:
                            read_log(process, log, ERROR)
                            if not child_is_alive(child_pid, process, executable):
                                raise RuntimeError("App exited before the requested exit action")
                            time.sleep(0.05)
                        launcher_ready_ns = None

                        def returned(t):
                            return t.count(exited) >= cycle and t.count(ready) >= cycle + 1

                        def observe_return():
                            nonlocal launcher_ready_ns
                            if launcher_ready_ns is None:
                                text = read_log(process, log, ERROR)
                                observed_ns = time.monotonic_ns()
                                if returned(text):
                                    launcher_ready_ns = observed_ns

                        if args.exit_method == "gesture":
                            exit_started_ns = swipe_exit(fd, orientation, args.app_rotation,
                                                         observe_return)
                        else:
                            exit_started_ns = time.monotonic_ns()
                            os.kill(child_pid, signal.SIGTERM)
                        if launcher_ready_ns is None:
                            _, launcher_ready_ns = wait_for(
                                process, log, returned,
                                f"{app_id} clean {args.exit_method} exit and Launcher return {cycle}",
                                error_pattern=ERROR)
                        if args.metrics:
                            record = {
                                "schema_version": 1,
                                "event": "app_switch",
                                "app_id": app_id,
                                "app_executable": str(executable),
                                "launcher_executable": str(args.launcher),
                                "launcher_renderer": args.launcher_renderer,
                                "orientation": orientation,
                                "app_rotation": args.app_rotation if orientation == "landscape" else 0,
                                "cycle": cycle,
                                "exit_method": args.exit_method,
                                "readiness_source": ("app_first_frame" if first_frame_ms is not None
                                                     else "gpu_context"),
                                "log_poll_interval_ms": POLL_SECONDS * 1000,
                                "release_to_started_ms": elapsed_ms(released_ns, started_ns),
                                "release_to_app_ready_ms": (
                                    elapsed_ms(released_ns, app_observed_ns)
                                    if first_frame_ms is not None else None),
                                "release_to_gpu_context_ms": (
                                    elapsed_ms(released_ns, app_observed_ns)
                                    if first_frame_ms is None else None),
                                "started_to_app_ready_ms": (
                                    elapsed_ms(started_ns, app_observed_ns)
                                    if first_frame_ms is not None else None),
                                "app_reported_first_frame_ms": first_frame_ms,
                                "sigterm_to_launcher_ready_ms": (
                                    elapsed_ms(exit_started_ns, launcher_ready_ns)
                                    if args.exit_method == "sigterm" else None),
                                "gesture_start_to_launcher_ready_ms": (
                                    elapsed_ms(exit_started_ns, launcher_ready_ns)
                                    if args.exit_method == "gesture" else None),
                                **memory,
                            }
                            with args.metrics.open("a") as metrics:
                                metrics.write(json.dumps(record, sort_keys=True) + "\n")
                        child_pid = None
                        print(f"PASS {app_id} {orientation} cycle={cycle} exit={args.exit_method}", flush=True)
                    process.terminate()
                    if process.wait(timeout=5) != 0:
                        raise RuntimeError("Launcher did not exit cleanly")
                    if ERROR.search(log.read_text(errors="replace")):
                        raise RuntimeError("Graphics/runtime error during shutdown")
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=8)
                        except subprocess.TimeoutExpired:
                            if child_pid and child_is_alive(child_pid, process, executable):
                                os.kill(child_pid, signal.SIGKILL)
                            process.kill()
                            process.wait()
                    print(f"--- {app_id} {orientation} log ---", flush=True)
                    print(log.read_text(errors="replace"), end="", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--launcher", type=Path, required=True)
    parser.add_argument("--launcher-renderer", choices=("software", "opengles", "nanovg"),
                        default="opengles",
                        help="Require the Launcher to report this renderer (default opengles)")
    parser.add_argument("--app", type=Path, action="append", required=True)
    parser.add_argument("--drm", default="/dev/dri/card0")
    parser.add_argument("--connector", type=int, default=44)
    parser.add_argument("--cycles", type=int, default=2)
    parser.add_argument("--seconds", type=float, default=2,
                        help="Minimum live time after app readiness (default 2)")
    parser.add_argument("--require-app-ready", action="store_true",
                        help="Require AppKit's UI-ready/first_frame_ms marker before testing exit")
    parser.add_argument("--exit-method", choices=("sigterm", "gesture"), default="sigterm",
                        help="Exit via SIGTERM or a bottom-up FIFO swipe (default sigterm)")
    parser.add_argument("--app-rotation", type=int, choices=(90, 270), default=90,
                        help="AppKit landscape LVGL rotation for gesture coordinates; "
                             "Launcher taps retain 90 degrees (default 90)")
    parser.add_argument("--metrics", type=Path,
                        help="Write switch timings as JSONL (overwrites PATH; completed cycles survive failures)")
    args = parser.parse_args()
    if os.uname().sysname != "Linux":
        parser.error("This test requires Linux and exclusive CM0 DRM ownership")
    if not 1 <= args.cycles <= 10 or not 0.2 <= args.seconds <= 30:
        parser.error("Use --cycles 1..10 and --seconds 0.2..30")
    args.launcher = args.launcher.resolve()
    args.app = [path.resolve() for path in args.app]
    for executable in [args.launcher, *args.app]:
        if not executable.is_file() or not os.access(executable, os.X_OK):
            parser.error(f"Not an executable file: {executable}")
    for executable in args.app:
        if executable.name not in APP_IDS:
            parser.error(f"Unknown app binary name: {executable.name}")
    if args.metrics:
        args.metrics = args.metrics.resolve()
        if args.metrics in [args.launcher, *args.app]:
            parser.error("Metrics output must not overwrite a tested executable")
        args.metrics.write_text("")
    with tempfile.TemporaryDirectory(prefix="lilygo-ui-app-drm-test-") as temporary:
        for index, executable in enumerate(args.app):
            directory = Path(temporary) / str(index)
            directory.mkdir()
            run_app(args, executable, directory)
    print(f"PASS: {len(args.app)} apps, {args.cycles} cycles per orientation, "
          f"VC4 app rendering and clean {args.launcher_renderer} Launcher handoff", flush=True)


if __name__ == "__main__":
    main()
