#!/usr/bin/env python3
"""Opt-in CM0 hardware smoke test; requires exclusive DRM ownership.

Uses temporary manifests and synthetic FIFO input. --child selects a successful
exit, nonzero exit, signal termination, or exec failure; every case must restore
the Launcher to a completed frame without ending its process.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from drm_test_support import input_fifo, ready_marker, tap_first_tile, wait_for


def prepare_child(root, behavior):
    if behavior == "success":
        return "/usr/bin/true", 0
    if behavior == "failure":
        return "/usr/bin/false", 1
    executable = root / "test-child"
    if behavior == "signal":
        executable.write_text('#!/bin/sh\nkill -TERM "$$"\nexit 99\n')
        expected_exit = 143
    else:
        # The file itself passes executable discovery, then execv fails because
        # its interpreter does not exist. Launcher must reap exit 127 and resume.
        executable.write_text(f"#!{root / 'missing-interpreter'}\n")
        expected_exit = 127
    executable.chmod(0o700)
    return str(executable), expected_exit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("launcher", type=Path)
    parser.add_argument("--drm", default="/dev/dri/card0")
    parser.add_argument("--connector", default="-1")
    parser.add_argument("--renderer", choices=("software", "opengles", "nanovg"),
                        help="Require the launcher to report this renderer")
    parser.add_argument("--cycles", type=int, default=3,
                        help="Application launch/return cycles per orientation, 1..20 (default 3)")
    parser.add_argument("--child", choices=("success", "failure", "signal", "exec-failure"),
                        default="success", help="Child exit behavior to test (default success)")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 20:
        parser.error("--cycles must be between 1 and 20")
    if os.uname().sysname != "Linux":
        parser.error("This test requires a Linux CM0 with a 568x1232 panel")
    args.launcher = args.launcher.resolve()
    if not args.launcher.is_file() or not os.access(args.launcher, os.X_OK):
        parser.error(f"Not an executable file: {args.launcher}")
    with tempfile.TemporaryDirectory(prefix="lilygo-ui-drm-test-") as directory:
        root = Path(directory)
        app_dir = root / "apps"
        app_dir.mkdir()
        executable, expected_exit = prepare_child(root, args.child)
        manifest = {
            "schema_version": 2,
            "id": "cc.lilygo.ui.GraphicsTest",
            "name": "Graphics Test",
            "icon": "settings",
            "order": -2147483648,
            "executable": executable,
        }
        (app_dir / "cc.lilygo.ui.GraphicsTest.json").write_text(json.dumps(manifest))
        fifo = root / "input"
        with input_fifo(fifo) as fd:
            for orientation in ("portrait", "landscape"):
                log = root / f"{orientation}.log"
                with log.open("w") as output:
                    process = subprocess.Popen(
                        [str(args.launcher), "--display", "drm",
                         "--drm", args.drm, "--drm-connector", args.connector,
                         "--input", str(fifo), "--app-dir", str(app_dir),
                         "--orientation", orientation],
                        stdout=output, stderr=subprocess.STDOUT,
                    )
                    try:
                        ready = ready_marker(orientation)
                        wait_for(process, log,
                                 lambda text: ready in text,
                                 f"{orientation} startup", poll_seconds=0.05)
                        if args.renderer and f"renderer={args.renderer}" not in log.read_text():
                            raise RuntimeError(
                                f"Expected renderer={args.renderer}:\n{log.read_text()}")
                        for cycle in range(1, args.cycles + 1):
                            tap_first_tile(fd, orientation)
                            marker = f"cc.lilygo.ui.GraphicsTest exited with code {expected_exit}\n"
                            wait_for(process, log,
                                     lambda text: text.count(marker) >= cycle
                                     and text.count(ready) >= cycle + 1,
                                     f"{orientation} {args.child} application return {cycle}",
                                     poll_seconds=0.05)
                        time.sleep(0.3)
                        process.terminate()
                        if process.wait(timeout=5) != 0:
                            raise RuntimeError(f"Unclean launcher shutdown:\n{log.read_text()}")
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.wait()
                        print(f"--- child={args.child} {orientation} log ---", flush=True)
                        print(log.read_text(errors="replace"), end="", flush=True)
    print(f"DRM launcher startup, {args.cycles} {args.child} application returns "
          f"per orientation (exit {expected_exit}), "
          "and shutdown passed in portrait and landscape")


if __name__ == "__main__":
    main()
