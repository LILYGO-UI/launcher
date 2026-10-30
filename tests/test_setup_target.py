#!/usr/bin/env python3
"""Opt-in target-board input and capture helpers for the first-boot wizard.

Run as root on a Linux target. These commands use the existing evdev and DRM
interfaces; they do not expose a Launcher control endpoint or reset accounts.
Passwords for ``type``, ``inspect-support`` and ``sudo-check`` are read only
from stdin. ``inspect-support`` prints only whether the input can be typed.
Keep shell tracing disabled, and do not put passwords in command-line arguments.
"""

import argparse
import ctypes
import errno
import fcntl
import mmap
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import time
import zlib


EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
KEY_CODES = {
    "ESC": 1, "BACKSPACE": 14, "TAB": 15, "ENTER": 28, "SPACE": 57,
    "HOME": 102, "UP": 103, "LEFT": 105, "RIGHT": 106,
    "END": 107, "DOWN": 108, "DELETE": 111,
}
LETTER_CODES = dict(zip("qwertyuiopasdfghjklzxcvbnm",
                       [16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
                        30, 31, 32, 33, 34, 35, 36, 37, 38,
                        44, 45, 46, 47, 48, 49, 50]))
CHAR_CODES = {letter: (code, False) for letter, code in LETTER_CODES.items()}
CHAR_CODES.update({letter.upper(): (code, True) for letter, code in LETTER_CODES.items()})
for plain, shifted, code in zip("1234567890-=[];'`,./\\", "!@#$%^&*()_+{}:\"~<>?|",
                                [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                 26, 27, 39, 40, 41, 51, 52, 53, 43]):
    CHAR_CODES[plain] = (code, False)
    CHAR_CODES[shifted] = (code, True)
CHAR_CODES[" "] = (57, False)


def event(fd, kind, code, value):
    os.write(fd, struct.pack("llHHi", 0, 0, kind, code, value))


def sync(fd):
    event(fd, EV_SYN, 0, 0)


def key(fd, code, delay=0.07):
    event(fd, EV_KEY, code, 1)
    sync(fd)
    time.sleep(delay)
    event(fd, EV_KEY, code, 0)
    sync(fd)
    time.sleep(delay)


def text_keys(text):
    """Collect every required key without revealing any input in errors."""
    if any(character not in CHAR_CODES for character in text):
        raise RuntimeError("Text contains characters unsupported by the Launcher US key map")
    required = {CHAR_CODES[character][0] for character in text}
    if any(CHAR_CODES[character][1] for character in text):
        required.add(42)  # KEY_LEFTSHIFT
    return required


def device_key_bits(fd):
    """Return real evdev capabilities, leaving FIFOs/test doubles unfiltered."""
    try:
        metadata = os.fstat(fd)
    except OSError as error:
        # Pure input-event tests can supply a synthetic fd with mocked writes.
        # An invalid fd used without a mock still fails when writing events.
        if error.errno == errno.EBADF:
            return None
        raise
    if not stat.S_ISCHR(metadata.st_mode) or os.major(metadata.st_rdev) != 13:
        return None
    bits = bytearray(96)  # Linux KEY_MAX (0x2ff), including the shift keys.
    try:
        fcntl.ioctl(fd, 0x80000000 | (len(bits) << 16) | (ord("E") << 8) | 0x21, bits)
    except OSError:
        raise RuntimeError("Cannot read input device key capabilities") from None
    return bits


def supports_keys(fd, required):
    bits = device_key_bits(fd)
    return bits is None or all(bits[code // 8] & (1 << (code % 8)) for code in required)


def validate_text_device(fd, text, replace=False):
    required = text_keys(text)
    if replace:
        required.update((KEY_CODES["END"], KEY_CODES["BACKSPACE"]))
    if not supports_keys(fd, required):
        raise RuntimeError("Input device lacks keys required for this operation; no input was sent")


def type_text(fd, text):
    # Linux silently discards injected keys missing from EVIOCGBIT(EV_KEY).
    # Check the complete string before sending even its first character.
    validate_text_device(fd, text)
    for character in text:
        code, shifted = CHAR_CODES[character]
        if shifted:
            event(fd, EV_KEY, 42, 1)
            sync(fd)
        key(fd, code)
        if shifted:
            event(fd, EV_KEY, 42, 0)
            sync(fd)


def read_secret():
    if sys.stdin.isatty():
        import getpass
        return getpass.getpass("Input (hidden): ")
    text = sys.stdin.read(4097)
    if len(text) > 4096:
        raise RuntimeError("Input exceeds the safety limit")
    return text.removesuffix("\n")


def keyboards():
    # Match AppKit's capability test and directory traversal order.
    for entry in os.scandir("/dev/input"):
        if not entry.name.startswith("event"):
            continue
        try:
            with open(entry.path, "rb", buffering=0) as device:
                bits = bytearray(96)
                fcntl.ioctl(device, 0x80000000 | (len(bits) << 16) | (ord("E") << 8) | 0x21, bits)
            if all(bits[code // 8] & (1 << (code % 8)) for code in (28, 30, 44, 57)):
                yield entry.path
        except OSError:
            continue


class Resources(ctypes.Structure):
    _fields_ = [("count_fbs", ctypes.c_int), ("fbs", ctypes.POINTER(ctypes.c_uint32)),
                ("count_crtcs", ctypes.c_int), ("crtcs", ctypes.POINTER(ctypes.c_uint32)),
                ("count_connectors", ctypes.c_int), ("connectors", ctypes.POINTER(ctypes.c_uint32)),
                ("count_encoders", ctypes.c_int), ("encoders", ctypes.POINTER(ctypes.c_uint32)),
                ("min_width", ctypes.c_uint32), ("max_width", ctypes.c_uint32),
                ("min_height", ctypes.c_uint32), ("max_height", ctypes.c_uint32)]


class CrtcPrefix(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in ("crtc_id", "buffer_id", "x", "y", "width", "height")]


class Framebuffer(ctypes.Structure):
    _fields_ = [("fb_id", ctypes.c_uint32), ("width", ctypes.c_uint32), ("height", ctypes.c_uint32),
                ("pixel_format", ctypes.c_uint32), ("modifier", ctypes.c_uint64), ("flags", ctypes.c_uint32),
                ("handles", ctypes.c_uint32 * 4), ("pitches", ctypes.c_uint32 * 4),
                ("offsets", ctypes.c_uint32 * 4)]


def png(path, width, height, rows):
    def chunk(kind, payload):
        return struct.pack("!I", len(payload)) + kind + payload + struct.pack("!I", zlib.crc32(kind + payload))
    encoded = b"\x89PNG\r\n\x1a\n"
    encoded += chunk(b"IHDR", struct.pack("!IIBBBBB", width, height, 8, 2, 0, 0, 0))
    encoded += chunk(b"IDAT", zlib.compress(b"".join(b"\0" + row for row in rows)))
    encoded += chunk(b"IEND", b"")
    Path(path).write_bytes(encoded)


def capture(card, destination):
    """Capture a linear XRGB/ARGB scanout without changing the DRM owner."""
    drm = ctypes.CDLL("libdrm.so.2", use_errno=True)
    for name, result in (("drmModeGetResources", ctypes.POINTER(Resources)),
                         ("drmModeGetCrtc", ctypes.POINTER(CrtcPrefix)),
                         ("drmModeGetFB2", ctypes.POINTER(Framebuffer))):
        getattr(drm, name).restype = result
    for name in ("drmModeFreeResources", "drmModeFreeCrtc", "drmModeFreeFB2"):
        getattr(drm, name).argtypes = [ctypes.c_void_p]
    fd = os.open(card, os.O_RDWR | os.O_CLOEXEC)
    resources = drm.drmModeGetResources(fd)
    if not resources:
        os.close(fd)
        raise RuntimeError("Cannot read DRM resources")
    try:
        for index in range(resources.contents.count_crtcs):
            crtc = drm.drmModeGetCrtc(fd, resources.contents.crtcs[index])
            if not crtc:
                continue
            buffer_id = crtc.contents.buffer_id
            drm.drmModeFreeCrtc(crtc)
            if not buffer_id:
                continue
            framebuffer = drm.drmModeGetFB2(fd, buffer_id)
            if not framebuffer:
                raise RuntimeError("Cannot read scanout framebuffer metadata")
            handles = set(framebuffer.contents.handles) - {0}
            try:
                fb = framebuffer.contents
                if fb.modifier != 0 or fb.pixel_format not in (0x34325258, 0x34325241):
                    raise RuntimeError("Scanout is not linear XRGB/ARGB8888; use a compatible board capture tool")
                if not fb.handles[0] or not 0 < fb.width <= 8192 or not 0 < fb.height <= 8192:
                    raise RuntimeError("Framebuffer handles unavailable (root/CAP_SYS_ADMIN required)")
                dma_fd = ctypes.c_int(-1)
                if drm.drmPrimeHandleToFD(fd, fb.handles[0], os.O_CLOEXEC, ctypes.byref(dma_fd)) != 0:
                    raise RuntimeError("Cannot export the scanout buffer: " + os.strerror(ctypes.get_errno()))
                try:
                    size = fb.offsets[0] + fb.pitches[0] * fb.height
                    with mmap.mmap(dma_fd.value, size, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ) as data:
                        # Synchronize DMA before the CPU reads GPU-rendered data.
                        fcntl.ioctl(dma_fd.value, 0x40086200, struct.pack("Q", 1))
                        try:
                            rows = []
                            for y in range(fb.height):
                                start = fb.offsets[0] + y * fb.pitches[0]
                                source = data[start:start + fb.width * 4]
                                row = bytearray(fb.width * 3)
                                row[0::3], row[1::3], row[2::3] = source[2::4], source[1::4], source[0::4]
                                rows.append(bytes(row))
                            png(destination, fb.width, fb.height, rows)
                        finally:
                            fcntl.ioctl(dma_fd.value, 0x40086200, struct.pack("Q", 5))
                finally:
                    os.close(dma_fd.value)
                print(f"Captured {fb.width}x{fb.height} to {destination}")
                return
            finally:
                drm.drmModeFreeFB2(framebuffer)
                for handle in handles:
                    fcntl.ioctl(fd, 0x40086409, struct.pack("II", handle, 0))
        raise RuntimeError("No active DRM scanout found")
    finally:
        drm.drmModeFreeResources(resources)
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("keyboards", help="List keyboards in AppKit discovery order")
    for name in ("key", "type", "tap", "inspect-support"):
        command = commands.add_parser(name)
        command.add_argument("device", help="Existing evdev event path or the test pointer FIFO")
        if name == "key":
            command.add_argument("key", choices=KEY_CODES)
            command.add_argument("--count", type=int, default=1)
        elif name in ("type", "inspect-support"):
            command.add_argument("--replace", action="store_true", help="Erase up to 128 existing characters first")
        else:
            command.add_argument("x", type=int)
            command.add_argument("y", type=int)
            command.add_argument("--landscape", action="store_true", help="Convert logical 1232x568 to native panel coordinates")
    capture_parser = commands.add_parser("capture")
    capture_parser.add_argument("destination")
    capture_parser.add_argument("--drm", default="/dev/dri/card0")
    sudo = commands.add_parser("sudo-check", help="Assert sudo returns uid 0, supplying stdin password if required")
    sudo.add_argument("username")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Target-board commands require Linux")
    if args.command == "keyboards":
        print("\n".join(keyboards()))
    elif args.command == "capture":
        capture(args.drm, args.destination)
    elif args.command == "sudo-check":
        secret = read_secret()
        result = subprocess.run(["runuser", "-u", args.username, "--", "sudo", "-k", "-S", "-p", "", "id", "-u"],
                                input=secret + "\n", text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode or result.stdout.strip() != "0":
            raise RuntimeError("sudo permission check failed")
        print("sudo returned uid 0")
    else:
        access = os.O_RDONLY if args.command == "inspect-support" else os.O_WRONLY
        fd = os.open(args.device, access | os.O_NONBLOCK | os.O_CLOEXEC)
        try:
            if args.command == "key":
                if not 1 <= args.count <= 256:
                    parser.error("--count must be between 1 and 256")
                if not supports_keys(fd, (KEY_CODES[args.key],)):
                    raise RuntimeError("Input device lacks the requested key; no input was sent")
                for _ in range(args.count):
                    key(fd, KEY_CODES[args.key])
            elif args.command == "type":
                text = read_secret()
                # Preflight before --replace can erase anything in the field.
                validate_text_device(fd, text, replace=args.replace)
                if args.replace:
                    key(fd, KEY_CODES["END"])
                    for _ in range(128):
                        key(fd, KEY_CODES["BACKSPACE"], delay=0.015)
                type_text(fd, text)
            elif args.command == "inspect-support":
                text = read_secret()
                try:
                    required = text_keys(text)
                except RuntimeError:
                    print("false")
                else:
                    if args.replace:
                        required.update((KEY_CODES["END"], KEY_CODES["BACKSPACE"]))
                    print("true" if supports_keys(fd, required) else "false")
            else:
                x, y = (args.y, 1231 - args.x) if args.landscape else (args.x, args.y)
                event(fd, EV_ABS, 0, x)
                event(fd, EV_ABS, 1, y)
                event(fd, EV_KEY, 330, 1)
                sync(fd)
                time.sleep(0.15)
                event(fd, EV_KEY, 330, 0)
                sync(fd)
        finally:
            os.close(fd)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as error:
        print(f"Target test failed: {error}", file=sys.stderr)
        sys.exit(1)
