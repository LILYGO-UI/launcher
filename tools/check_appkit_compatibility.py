#!/usr/bin/env python3
"""Check embedded AppKit copies against the canonical TCA8418 contract."""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CM0_ROOT = ROOT.parent
CANONICAL = CM0_ROOT / "lilygo-cm0-appkit"
REQUIRED = (
    "include/cm0/input.h",
    "src/input.cpp",
    "src/runtime.cpp",
    "patches/lvgl-9.5.0-keyboard.patch",
    "tests/test_runtime_input.cpp",
    "src/system_status.cpp",
    "tests/test_system_status.cpp",
    "docs/keyboard-input.md",
    "docs/tca8418-keyboard-contract.md",
)
APP_SOURCE_PATTERNS = ("lv_evdev_create", "EVIOCG")
APP_DEVICE_PATH = "/dev/input"
APP_DEVICE_PATH_ALLOWLIST = {
    "src/platform/multitouch_reader.cpp",
    "src/pages/keyboard/keyboard_source.cpp",
    "src/pages/keyboard/keyboard_source.hpp",
    "src/pages/assistant/hardware_mic_input.cpp",
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()[:12]


def git(path: Path, *args: str) -> str | None:
    result = subprocess.run(
        ["git", "-C", str(path), *args],
        text=True,
        capture_output=True,
    )
    return result.stdout.strip() if result.returncode == 0 else None


def check_copy(app: Path, canonical_head: str | None) -> tuple[str, list[str]]:
    sdk = app / "third_party/cm0-appkit"
    problems: list[str] = []
    if not sdk.is_dir():
        return "missing", ["third_party/cm0-appkit is missing"]

    sdk_head = git(sdk, "rev-parse", "HEAD")
    state = "git" if sdk_head else "embedded-copy"
    if canonical_head and sdk_head and sdk_head != canonical_head:
        problems.append(f"git HEAD {sdk_head[:12]} != canonical {canonical_head[:12]}")
    for relative in REQUIRED:
        source = CANONICAL / relative
        target = sdk / relative
        if not target.is_file():
            problems.append(f"missing {relative}")
        elif source.is_file() and target.read_bytes() != source.read_bytes():
            problems.append(f"differs {relative} ({digest(target)} != {digest(source)})")

    source_files = [
        path for path in (app / "src").rglob("*") if path.is_file()
    ] if (app / "src").is_dir() else []
    for path in source_files:
        text = path.read_text(errors="ignore")
        relative = str(path.relative_to(app))
        owns_device_path = APP_DEVICE_PATH in text and relative not in APP_DEVICE_PATH_ALLOWLIST
        if relative not in APP_DEVICE_PATH_ALLOWLIST and (
            owns_device_path or any(pattern in text for pattern in APP_SOURCE_PATTERNS)
        ):
            problems.append(f"application owns evdev path: {relative}")
    return state, problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--strict", action="store_true", help="exit 1 on any incompatibility")
    parser.add_argument("--app", action="append", help="check only this app directory name")
    args = parser.parse_args()

    canonical_head = git(CANONICAL, "rev-parse", "HEAD")
    if not canonical_head:
        print(f"canonical AppKit is not a Git checkout: {CANONICAL}", file=sys.stderr)
        return 2
    names = set(args.app or [])
    apps = sorted(
        path for path in CM0_ROOT.glob("lilygo-ui-*")
        if path.name != "lilygo-ui-mesa-vc4" and (not names or path.name in names)
    )
    failures = 0
    print(f"canonical AppKit: {canonical_head} ({CANONICAL})")
    for app in apps:
        state, problems = check_copy(app, canonical_head)
        if problems:
            failures += 1
            print(f"FAIL {app.name} [{state}]")
            for problem in problems:
                print(f"  - {problem}")
        else:
            print(f"OK   {app.name} [{state}]")
    print(f"checked {len(apps)} apps; {failures} incompatible")
    return 1 if args.strict and failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
