"""Shared input and log helpers for the opt-in CM0 DRM hardware tests.

The tests run on the target, so Linux input_event uses the native ABI layout.
Timestamps observe process logs or input submission, never panel scanout.
"""

from contextlib import contextmanager
import os
import struct
import time


POLL_SECONDS = 0.01
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
SYN_REPORT, ABS_X, ABS_Y, BTN_TOUCH = 0, 0, 1, 330


@contextmanager
def input_fifo(path):
    os.mkfifo(path)
    # Keep both ends open across Launcher/AppKit input delete/recreate cycles.
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    try:
        yield fd
    finally:
        os.close(fd)


def event(fd, kind, code, value):
    os.write(fd, struct.pack("llHHi", 0, 0, kind, code, value))


def position(fd, x, y):
    event(fd, EV_ABS, ABS_X, x)
    event(fd, EV_ABS, ABS_Y, y)


def touch(fd, pressed):
    event(fd, EV_KEY, BTN_TOUCH, int(pressed))


def sync(fd):
    event(fd, EV_SYN, SYN_REPORT, 0)


def tap(fd, x, y):
    position(fd, x, y)
    touch(fd, True)
    sync(fd)
    time.sleep(0.15)
    # Preserve the timing origin immediately before submitting the release.
    released_ns = time.monotonic_ns()
    touch(fd, False)
    sync(fd)
    return released_ns


def physical_position(x, y, orientation, rotation=90):
    # Invert LVGL's display rotation for native 568x1232 evdev coordinates.
    if orientation == "portrait":
        return x, y
    if rotation == 90:
        return y, 1232 - x - 1
    if rotation == 270:
        return 568 - y - 1, x
    raise ValueError(f"Unsupported landscape rotation: {rotation}")


def tap_first_tile(fd, orientation):
    logical_x = 104 if orientation == "portrait" else 122
    return tap(fd, *physical_position(logical_x, 155, orientation))


def ready_marker(orientation):
    dimensions = "568x1232" if orientation == "portrait" else "1232x568"
    return f"UI ready at {dimensions}"


def read_log(process, path, error_pattern=None):
    text = path.read_text(errors="replace")
    if process.poll() is not None:
        raise RuntimeError(f"Launcher exited unexpectedly ({process.returncode}):\n{text}")
    if error_pattern:
        error = error_pattern.search(text)
        if error:
            raise RuntimeError(f"Graphics/runtime error in log: {error.group()}\n{text}")
    return text


def wait_for(process, log, predicate, description, *,
             poll_seconds=POLL_SECONDS, error_pattern=None):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        text = read_log(process, log, error_pattern)
        observed_ns = time.monotonic_ns()
        if predicate(text):
            return text, observed_ns
        time.sleep(poll_seconds)
    raise RuntimeError(f"Timed out during {description}:\n{log.read_text(errors='replace')}")
